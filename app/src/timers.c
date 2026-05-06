// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Implementation of the daily recompute + per-event one-shot timer
// scheduler. See timers.h for the public-API contract.
//
// Threading model: every callback runs on the GLib main loop thread.
// No locks needed — the per-event timer-handle table is only ever
// touched from main-loop callbacks.
//
// Event slots. Each entry binds an event-topic id (the same string
// declared in settings/events.json and registered with the AXEvent
// engine via ACAP_EVENTS_Add_Event) to the way its computed fire time
// is derived from a solar_events_t struct. M3 covers the full FR-3
// solar suite: sunrise/sunset, solar noon/midnight, and three
// twilight zeniths in dawn/dusk pairs.

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
// Slot configuration
// ---------------------------------------------------------------------

// Which field of solar_events_t a given event maps to.
typedef enum {
    SLOT_RISE,         // sunrise / dawn  — needs a zenith
    SLOT_SET,          // sunset  / dusk  — needs a zenith
    SLOT_SOLAR_NOON,   // upper culmination
    SLOT_SOLAR_MIDNIGHT // lower culmination
} slot_kind_t;

typedef struct {
    const char* event_id;     // matches settings/events.json id
    slot_kind_t kind;
    double      zenith_deg;   // ignored for SOLAR_NOON / SOLAR_MIDNIGHT
    GSource*    timer;        // NULL when not currently armed
} event_slot_t;

// One slot per registered event topic.
//
// Note. solar_compute can be called once per zenith and re-used to
// fill multiple slots; recompute_today() groups slots by zenith below
// so we only compute three times (sunrise/sunset, civil, nautical,
// astronomical — and noon/midnight come along for free with any of
// them since they don't depend on zenith).
static event_slot_t event_slots[] = {
    { "sunrise",      SLOT_RISE,           SOLAR_ZENITH_SUNRISE_SUNSET,        NULL },
    { "sunset",       SLOT_SET,            SOLAR_ZENITH_SUNRISE_SUNSET,        NULL },
    { "sunnoon",      SLOT_SOLAR_NOON,     0.0,                                NULL },
    { "sunmidnight",  SLOT_SOLAR_MIDNIGHT, 0.0,                                NULL },
    { "civildawn",    SLOT_RISE,           SOLAR_ZENITH_CIVIL_TWILIGHT,        NULL },
    { "civildusk",    SLOT_SET,            SOLAR_ZENITH_CIVIL_TWILIGHT,        NULL },
    { "nauticaldawn", SLOT_RISE,           SOLAR_ZENITH_NAUTICAL_TWILIGHT,     NULL },
    { "nauticaldusk", SLOT_SET,            SOLAR_ZENITH_NAUTICAL_TWILIGHT,     NULL },
    { "astrodawn",    SLOT_RISE,           SOLAR_ZENITH_ASTRONOMICAL_TWILIGHT, NULL },
    { "astrodusk",    SLOT_SET,            SOLAR_ZENITH_ASTRONOMICAL_TWILIGHT, NULL },
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
    if (remaining <= 0) remaining = 86400;
    return remaining;
}

typedef struct { const char* event_id; } fire_args_t;

static gboolean event_fire_callback(gpointer user_data) {
    fire_args_t* args = (fire_args_t*)user_data;
    LOG("Firing event topic '%s'", args->event_id);
    int rc = ACAP_EVENTS_Fire(args->event_id);
    if (rc != 0)
        LOG_WARN("ACAP_EVENTS_Fire('%s') returned %d", args->event_id, rc);
    return G_SOURCE_REMOVE;
}

// Arm one event slot for the given absolute UTC `when`. SOLAR_NO_EVENT
// or a time in the past leaves the slot disarmed. SOLAR_NO_EVENT
// emits an INFO log per FR-3.8.
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
    fire_args_t* args = g_new(fire_args_t, 1);
    args->event_id = slot->event_id;
    g_source_set_callback(src, event_fire_callback, args, g_free);
    g_source_attach(src, NULL);
    slot->timer = src;

    LOG("event '%s': armed for UTC %lld (in %u s)",
        slot->event_id, (long long)when, delay);
}

// Pick the right time_t out of `e` for a given slot kind.
static time_t time_for_slot(const event_slot_t* slot, const solar_events_t* e) {
    switch (slot->kind) {
        case SLOT_RISE:           return e->sunrise;
        case SLOT_SET:            return e->sunset;
        case SLOT_SOLAR_NOON:     return e->solar_noon;
        case SLOT_SOLAR_MIDNIGHT: return e->solar_midnight;
    }
    return SOLAR_NO_EVENT;  // unreachable
}

// ---------------------------------------------------------------------
// Recompute pipeline
// ---------------------------------------------------------------------

static gboolean midnight_timer_callback(gpointer user_data);

// One full daily recompute: read lat/lon, compute solar events at every
// zenith we care about, arm each slot's timer.
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

    struct tm tm_local;
    localtime_r(&now, &tm_local);
    int year  = tm_local.tm_year + 1900;
    int month = tm_local.tm_mon  + 1;
    int day   = tm_local.tm_mday;

    LOG("Recomputing solar events for lat=%f lon=%f date=%04d-%02d-%02d",
        lat, lon, year, month, day);

    // Compute once per zenith. Solar noon/midnight are zenith-
    // independent, so any compute call's `solar_noon` / `solar_midnight`
    // fields are usable — we take them from the standard sunrise/sunset
    // call below.
    solar_events_t e_std = {0}, e_civil = {0}, e_naut = {0}, e_astro = {0};

    if (solar_compute(lat, lon, year, month, day,
                      SOLAR_ZENITH_SUNRISE_SUNSET, &e_std) != 0) {
        LOG_WARN("solar_compute (standard) failed");
        return -1;
    }
    (void)solar_compute(lat, lon, year, month, day,
                        SOLAR_ZENITH_CIVIL_TWILIGHT, &e_civil);
    (void)solar_compute(lat, lon, year, month, day,
                        SOLAR_ZENITH_NAUTICAL_TWILIGHT, &e_naut);
    (void)solar_compute(lat, lon, year, month, day,
                        SOLAR_ZENITH_ASTRONOMICAL_TWILIGHT, &e_astro);

    LOG("solar_noon=%lld solar_midnight=%lld",
        (long long)e_std.solar_noon, (long long)e_std.solar_midnight);

    // Arm each slot from the matching zenith's results.
    for (size_t i = 0; i < EVENT_SLOT_COUNT; i++) {
        event_slot_t* slot = &event_slots[i];
        const solar_events_t* src = &e_std;
        if (slot->zenith_deg == SOLAR_ZENITH_CIVIL_TWILIGHT)        src = &e_civil;
        else if (slot->zenith_deg == SOLAR_ZENITH_NAUTICAL_TWILIGHT) src = &e_naut;
        else if (slot->zenith_deg == SOLAR_ZENITH_ASTRONOMICAL_TWILIGHT) src = &e_astro;
        arm_event_slot(slot, time_for_slot(slot, src), now);
    }
    return 0;
}

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
    return G_SOURCE_REMOVE;
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
