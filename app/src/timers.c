// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Implementation of the daily recompute + per-event one-shot timer
// scheduler. See timers.h for the public-API contract.
//
// Threading model: every callback runs on the GLib main loop thread.
// No locks needed; the per-event timer-handle table is only ever
// touched from main-loop callbacks.

#define _GNU_SOURCE
#include "timers.h"
#include "astro/solar.h"
#include "acap/ACAP.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define LOG(fmt, args...)      do { syslog(LOG_INFO,    fmt, ## args); } while (0)
#define LOG_WARN(fmt, args...) do { syslog(LOG_WARNING, fmt, ## args); } while (0)

// ---------------------------------------------------------------------
// State (single GMainContext, single thread, no locks)
// ---------------------------------------------------------------------

// One slot per registered event topic. M2 ships two: sunrise, sunset.
// M3+ will grow to ten solar topics; the array is sized for the full
// solar suite so we don't rewrite this when adding events.
typedef struct {
    const char* event_id;     // matches settings/events.json id and
                              // the topic registered via
                              // ACAP_EVENTS_Add_Event
    GSource*    timer;        // NULL when no event is armed today
                              // (e.g. polar night/day)
} event_slot_t;

static event_slot_t event_slots[] = {
    { "sunrise", NULL },
    { "sunset",  NULL },
    // Solar noon, twilights, etc. land in M3.
};
#define EVENT_SLOT_COUNT (sizeof(event_slots) / sizeof(event_slots[0]))

static GSource* midnight_timer = NULL;

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

static void cancel_source(GSource** src) {
    if (*src) {
        g_source_destroy(*src);
        g_source_unref(*src);
        *src = NULL;
    }
}

// Seconds from `now` until the next local civil midnight (00:00:00 in
// the camera's timezone). Always returns a positive value bounded by
// 86400.
static int seconds_until_local_midnight(time_t now) {
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    int seconds_into_day = tm_local.tm_hour * 3600
                         + tm_local.tm_min  * 60
                         + tm_local.tm_sec;
    int remaining = 86400 - seconds_into_day;
    if (remaining <= 0) remaining = 86400;  // defensive; should not happen
    return remaining;
}

// Fire a single event topic by id. Wrapped so per-event callbacks
// don't have to know about ACAP's API directly.
typedef struct {
    const char* event_id;
} fire_args_t;

static gboolean event_fire_callback(gpointer user_data) {
    fire_args_t* args = (fire_args_t*)user_data;
    LOG("Firing event topic '%s'", args->event_id);
    int rc = ACAP_EVENTS_Fire(args->event_id);
    if (rc != 0)
        LOG_WARN("ACAP_EVENTS_Fire('%s') returned %d", args->event_id, rc);
    // Slot's timer pointer is now stale; the slot's owner clears it
    // by walking the table on the next recompute. Returning
    // G_SOURCE_REMOVE both detaches the source and releases its ref.
    return G_SOURCE_REMOVE;
}

// Arm one event slot for the given absolute UTC `when`. If `when` is
// in the past or equal to SOLAR_NO_EVENT, the slot is left disarmed.
static void arm_event_slot(event_slot_t* slot, time_t when, time_t now) {
    cancel_source(&slot->timer);

    if (when == SOLAR_NO_EVENT) {
        LOG("event '%s': no event today (polar/extreme zenith), skipping",
            slot->event_id);
        return;
    }
    if (when <= now) {
        LOG("event '%s': already passed today (%lld <= %lld), skipping",
            slot->event_id, (long long)when, (long long)now);
        return;
    }

    guint delay = (guint)(when - now);
    GSource* src = g_timeout_source_new_seconds(delay);
    if (!src) {
        LOG_WARN("event '%s': failed to allocate timer source", slot->event_id);
        return;
    }

    // The fire_args lifetime spans the timer; freed by GLib via the
    // GDestroyNotify when the source is destroyed.
    fire_args_t* args = g_new(fire_args_t, 1);
    args->event_id = slot->event_id;

    g_source_set_callback(src, event_fire_callback, args, g_free);
    g_source_attach(src, NULL);
    slot->timer = src;

    LOG("event '%s': armed for UTC %lld (in %u s)",
        slot->event_id, (long long)when, delay);
}

// ---------------------------------------------------------------------
// Recompute pipeline
// ---------------------------------------------------------------------

static gboolean midnight_timer_callback(gpointer user_data);

// One full daily recompute: read lat/lon from the camera's geolocation
// service, compute today's solar events, arm each per-event timer.
// Does NOT (re)arm the midnight timer — call sites do that separately.
static int recompute_today(void) {
    double lat = ACAP_DEVICE_Latitude();
    double lon = ACAP_DEVICE_Longitude();

    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        LOG_WARN("Invalid camera geolocation lat=%f lon=%f; events disarmed",
                 lat, lon);
        for (size_t i = 0; i < EVENT_SLOT_COUNT; i++)
            cancel_source(&event_slots[i].timer);
        return -1;
    }

    time_t now;
    time(&now);

    // Compute "today" in local civil time. solar_compute treats the
    // input date as UTC, but for habitable longitudes the local civil
    // date and the UTC date that contains solar noon coincide — and
    // sunrise/sunset always fall around solar noon — so passing the
    // local date works.
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    int year  = tm_local.tm_year + 1900;
    int month = tm_local.tm_mon  + 1;
    int day   = tm_local.tm_mday;

    solar_events_t e;
    int rc = solar_compute(lat, lon, year, month, day,
                           SOLAR_ZENITH_SUNRISE_SUNSET, &e);
    if (rc != 0) {
        LOG_WARN("solar_compute failed for lat=%f lon=%f date=%04d-%02d-%02d",
                 lat, lon, year, month, day);
        return -1;
    }

    LOG("Recomputed solar events for lat=%f lon=%f date=%04d-%02d-%02d: "
        "sunrise=%lld sunset=%lld noon=%lld",
        lat, lon, year, month, day,
        (long long)e.sunrise, (long long)e.sunset, (long long)e.solar_noon);

    // Pair the named events to the computed times. Order must match
    // the event_slots[] declaration above.
    time_t event_times[EVENT_SLOT_COUNT] = { e.sunrise, e.sunset };

    for (size_t i = 0; i < EVENT_SLOT_COUNT; i++)
        arm_event_slot(&event_slots[i], event_times[i], now);

    return 0;
}

// Arm the next-midnight timer. The callback recomputes the day's
// events and rearms itself.
static void arm_midnight_timer(void) {
    cancel_source(&midnight_timer);

    time_t now;
    time(&now);
    int delay = seconds_until_local_midnight(now);

    GSource* src = g_timeout_source_new_seconds((guint)delay);
    if (!src) {
        LOG_WARN("Failed to allocate midnight timer source");
        return;
    }
    g_source_set_callback(src, midnight_timer_callback, NULL, NULL);
    g_source_attach(src, NULL);
    midnight_timer = src;

    LOG("Midnight recompute armed for %d s from now", delay);
}

static gboolean midnight_timer_callback(gpointer user_data) {
    (void)user_data;
    LOG("Local midnight reached; recomputing daily events");
    (void)recompute_today();
    arm_midnight_timer();
    return G_SOURCE_REMOVE;  // detach the just-fired source; new one is armed
}

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

int timers_init(void) {
    int rc = recompute_today();
    arm_midnight_timer();
    return rc;
}

int timers_recompute_now(void) {
    return recompute_today();
}

void timers_cleanup(void) {
    cancel_source(&midnight_timer);
    for (size_t i = 0; i < EVENT_SLOT_COUNT; i++)
        cancel_source(&event_slots[i].timer);
}
