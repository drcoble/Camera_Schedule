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
// is derived from a solar_events_t / lunar_events_t struct.
//
// Three distinct scheduler patterns coexist here:
//
//   * Daily slots (`event_slots[]`). Solar full FR-3 suite plus lunar
//     rise/set/transit/anti-transit. These all fit the midnight-
//     recompute cadence: at local civil midnight we recompute every
//     daily quantity for the new UTC date and arm one-shot timers.
//
//   * Phase slots (`phase_slots[]`). The four lunar phases (new /
//     first quarter / full / last quarter) are point-in-time instants
//     ~29.5 days apart, so they do NOT fit the midnight-recompute
//     cadence. Each phase slot is armed once at boot for the next
//     instant of its kind, and re-armed in its own fire callback by
//     querying lunar_next_phase() with `after = now`. Phases are
//     observer-independent (no lat/lon dependency), so
//     timers_recompute_now() does NOT touch them — only the daily
//     slots get re-armed when geolocation changes.
//
//   * Season slots (`season_slots[]`). The four annual seasonal
//     events (March equinox, June solstice, September equinox,
//     December solstice) are point-in-time instants ~91-92 days
//     apart. Same scheduling shape as phase slots: armed once at
//     boot, re-armed in fire callback via seasonal_next(). Likewise
//     observer-independent — timers_recompute_now() does NOT touch
//     them. Hemisphere-aware *labels* (FR-5.2) are applied at event-
//     registration time in main.c, not here; this module only
//     schedules the firing instant.

#define _GNU_SOURCE
#include "timers.h"
#include "astro/solar.h"
#include "astro/lunar.h"
#include "astro/seasonal.h"
#include "acap/ACAP.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define LOG(fmt, args...)      do { syslog(LOG_INFO,    fmt, ## args); } while (0)
#define LOG_WARN(fmt, args...) do { syslog(LOG_WARNING, fmt, ## args); } while (0)

// ---------------------------------------------------------------------
// Daily slot configuration (solar + lunar rise/set/transit/anti-transit)
// ---------------------------------------------------------------------

// Which field of solar_events_t / lunar_events_t a given event maps to.
typedef enum {
    SLOT_RISE,                // sunrise / dawn  — needs a zenith
    SLOT_SET,                 // sunset  / dusk  — needs a zenith
    SLOT_SOLAR_NOON,          // upper culmination
    SLOT_SOLAR_MIDNIGHT,      // lower culmination
    SLOT_LUNAR_RISE,          // moonrise
    SLOT_LUNAR_SET,           // moonset
    SLOT_LUNAR_TRANSIT,       // upper lunar culmination
    SLOT_LUNAR_ANTI_TRANSIT   // lower lunar culmination
} slot_kind_t;

typedef struct {
    const char* event_id;     // matches settings/events.json id
    slot_kind_t kind;
    double      zenith_deg;   // ignored for non-solar-rise/set kinds
    GSource*    timer;        // NULL when not currently armed
} event_slot_t;

// One slot per registered daily event topic.
//
// Note. solar_compute can be called once per zenith and re-used to
// fill multiple slots; recompute_today() groups slots by zenith below
// so we only compute four times for solar (sunrise/sunset, civil,
// nautical, astronomical — and noon/midnight come along for free with
// any of them since they don't depend on zenith). For lunar, one call
// to lunar_compute_daily() fills all four lunar slots at once.
static event_slot_t event_slots[] = {
    { "sunrise",      SLOT_RISE,                SOLAR_ZENITH_SUNRISE_SUNSET,        NULL },
    { "sunset",       SLOT_SET,                 SOLAR_ZENITH_SUNRISE_SUNSET,        NULL },
    { "sunnoon",      SLOT_SOLAR_NOON,          0.0,                                NULL },
    { "sunmidnight",  SLOT_SOLAR_MIDNIGHT,      0.0,                                NULL },
    { "civildawn",    SLOT_RISE,                SOLAR_ZENITH_CIVIL_TWILIGHT,        NULL },
    { "civildusk",    SLOT_SET,                 SOLAR_ZENITH_CIVIL_TWILIGHT,        NULL },
    { "nauticaldawn", SLOT_RISE,                SOLAR_ZENITH_NAUTICAL_TWILIGHT,     NULL },
    { "nauticaldusk", SLOT_SET,                 SOLAR_ZENITH_NAUTICAL_TWILIGHT,     NULL },
    { "astrodawn",    SLOT_RISE,                SOLAR_ZENITH_ASTRONOMICAL_TWILIGHT, NULL },
    { "astrodusk",    SLOT_SET,                 SOLAR_ZENITH_ASTRONOMICAL_TWILIGHT, NULL },
    { "moonrise",     SLOT_LUNAR_RISE,          0.0,                                NULL },
    { "moonset",      SLOT_LUNAR_SET,           0.0,                                NULL },
    { "moonnoon",     SLOT_LUNAR_TRANSIT,       0.0,                                NULL },
    { "moonmidnight", SLOT_LUNAR_ANTI_TRANSIT,  0.0,                                NULL },
};
#define EVENT_SLOT_COUNT (sizeof(event_slots) / sizeof(event_slots[0]))

// ---------------------------------------------------------------------
// Phase slot configuration (lunar phases — new / 1Q / full / 3Q)
// ---------------------------------------------------------------------

// Phase slots use a separate machinery from daily slots:
//   * They are observer-independent (no lat/lon, no zenith).
//   * They are not daily — instances of each kind are ~29.5 days apart.
//   * They re-arm themselves in the fire callback by calling
//     lunar_next_phase(now, kind, ...) again.
typedef struct {
    const char*   event_id;   // matches settings/events.json id
    lunar_phase_t kind;
    GSource*      timer;      // NULL when not currently armed
} phase_slot_t;

static phase_slot_t phase_slots[] = {
    { "newmoon",      LUNAR_PHASE_NEW,           NULL },
    { "firstquarter", LUNAR_PHASE_FIRST_QUARTER, NULL },
    { "fullmoon",     LUNAR_PHASE_FULL,          NULL },
    { "lastquarter",  LUNAR_PHASE_LAST_QUARTER,  NULL },
};
#define PHASE_SLOT_COUNT (sizeof(phase_slots) / sizeof(phase_slots[0]))

// ---------------------------------------------------------------------
// Season slot configuration (equinoxes + solstices)
// ---------------------------------------------------------------------

// Same shape as phase slots — observer-independent, ~91-day cadence,
// re-arms in fire callback by querying seasonal_next() again.
typedef struct {
    const char*     event_id;
    seasonal_kind_t kind;
    GSource*        timer;
} season_slot_t;

static season_slot_t season_slots[] = {
    { "marchequinox",     SEASONAL_MARCH_EQUINOX,     NULL },
    { "junesolstice",     SEASONAL_JUNE_SOLSTICE,     NULL },
    { "septemberequinox", SEASONAL_SEPTEMBER_EQUINOX, NULL },
    { "decembersolstice", SEASONAL_DECEMBER_SOLSTICE, NULL },
};
#define SEASON_SLOT_COUNT (sizeof(season_slots) / sizeof(season_slots[0]))

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

// Arm one daily-event slot for the given absolute UTC `when`.
// SOLAR_NO_EVENT / LUNAR_NO_EVENT (both equal (time_t)-1) or a time in
// the past leaves the slot disarmed. NO_EVENT emits an INFO log per
// FR-3.8 / FR-4.5 graceful-degradation guidance.
static void arm_event_slot(event_slot_t* slot, time_t when, time_t now) {
    cancel_source(&slot->timer);

    // SOLAR_NO_EVENT and LUNAR_NO_EVENT are both defined as (time_t)-1,
    // so a single sentinel check covers both daily-event domains.
    if (when == SOLAR_NO_EVENT) {
        LOG("event '%s': no event today (polar/extreme zenith or no horizon crossing), skipping",
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

// Pick the right time_t out of the supplied solar/lunar event structs
// for a given slot kind. The lunar struct is allowed to be NULL when
// only solar slots are being armed; the unreachable default in the
// switch protects against future enum additions.
static time_t time_for_slot(const event_slot_t* slot,
                            const solar_events_t* s,
                            const lunar_events_t* l) {
    switch (slot->kind) {
        case SLOT_RISE:               return s->sunrise;
        case SLOT_SET:                return s->sunset;
        case SLOT_SOLAR_NOON:         return s->solar_noon;
        case SLOT_SOLAR_MIDNIGHT:     return s->solar_midnight;
        case SLOT_LUNAR_RISE:         return l ? l->moonrise          : SOLAR_NO_EVENT;
        case SLOT_LUNAR_SET:          return l ? l->moonset           : SOLAR_NO_EVENT;
        case SLOT_LUNAR_TRANSIT:      return l ? l->lunar_transit     : SOLAR_NO_EVENT;
        case SLOT_LUNAR_ANTI_TRANSIT: return l ? l->lunar_anti_transit: SOLAR_NO_EVENT;
    }
    return SOLAR_NO_EVENT;  // unreachable
}

// True iff the slot's kind pulls its time out of lunar_events_t.
static int slot_is_lunar(const event_slot_t* slot) {
    return slot->kind == SLOT_LUNAR_RISE
        || slot->kind == SLOT_LUNAR_SET
        || slot->kind == SLOT_LUNAR_TRANSIT
        || slot->kind == SLOT_LUNAR_ANTI_TRANSIT;
}

// ---------------------------------------------------------------------
// Phase slot scheduler (lunar phases)
// ---------------------------------------------------------------------

typedef struct { phase_slot_t* slot; } phase_fire_args_t;

static void arm_phase_slot(phase_slot_t* slot);

static gboolean phase_fire_callback(gpointer user_data) {
    phase_fire_args_t* args = (phase_fire_args_t*)user_data;
    phase_slot_t* slot = args->slot;
    LOG("Firing lunar-phase event '%s'", slot->event_id);
    int rc = ACAP_EVENTS_Fire(slot->event_id);
    if (rc != 0)
        LOG_WARN("ACAP_EVENTS_Fire('%s') returned %d", slot->event_id, rc);

    // Re-arm for the next instance of this phase kind. arm_phase_slot
    // calls cancel_source on the existing slot->timer first;
    // g_source_destroy + g_source_unref is safe on a source that's
    // mid-dispatch (the main context still holds its own ref until the
    // callback returns G_SOURCE_REMOVE), so the dispatch completes
    // cleanly even though we've torn down the slot's bookkeeping.
    arm_phase_slot(slot);
    return G_SOURCE_REMOVE;
}

// Compute and arm the next-occurrence one-shot timer for a phase slot.
// Idempotent; safe to call when the slot is already armed (the
// existing source is cancelled first).
static void arm_phase_slot(phase_slot_t* slot) {
    cancel_source(&slot->timer);

    time_t now;
    time(&now);

    time_t next = 0;
    int rc = lunar_next_phase(now, slot->kind, &next);
    if (rc != 0) {
        LOG_WARN("lunar_next_phase('%s') failed (rc=%d); slot disarmed",
                 slot->event_id, rc);
        return;
    }

    // Defensive: the lunar_next_phase contract guarantees the result
    // is strictly after `now`, but if numerical drift ever produced a
    // past value we'd loop-fire forever. Refuse to arm in that case
    // and emit a warning so the issue is visible in syslog.
    if (next <= now) {
        LOG_WARN("lunar_next_phase('%s') returned past instant %lld <= now %lld; not arming",
                 slot->event_id, (long long)next, (long long)now);
        return;
    }

    // The next phase is at most ~29.5 days = ~2.55M seconds away,
    // comfortably within guint range on every platform that can host
    // an ACAP. No 24h cap (unlike daily slots, which are implicitly
    // capped by the midnight recompute).
    guint delay = (guint)(next - now);
    GSource* src = g_timeout_source_new_seconds(delay);
    if (!src) {
        LOG_WARN("phase '%s': failed to allocate timer source", slot->event_id);
        return;
    }
    phase_fire_args_t* args = g_new(phase_fire_args_t, 1);
    args->slot = slot;
    g_source_set_callback(src, phase_fire_callback, args, g_free);
    g_source_attach(src, NULL);
    slot->timer = src;

    LOG("phase '%s': armed for UTC %lld (in %u s, ~%.2f days)",
        slot->event_id, (long long)next, delay, (double)delay / 86400.0);
}

// ---------------------------------------------------------------------
// Season slot scheduler (equinoxes + solstices)
// ---------------------------------------------------------------------

typedef struct { season_slot_t* slot; } season_fire_args_t;

static void arm_season_slot(season_slot_t* slot);

static gboolean season_fire_callback(gpointer user_data) {
    season_fire_args_t* args = (season_fire_args_t*)user_data;
    season_slot_t* slot = args->slot;
    LOG("Firing seasonal event '%s'", slot->event_id);
    int rc = ACAP_EVENTS_Fire(slot->event_id);
    if (rc != 0)
        LOG_WARN("ACAP_EVENTS_Fire('%s') returned %d", slot->event_id, rc);

    // Re-arm for the next instance of this seasonal kind. Same teardown
    // semantics as the phase callback — destroy/unref of a mid-dispatch
    // source is safe because the main context still holds its own ref
    // until G_SOURCE_REMOVE returns.
    arm_season_slot(slot);
    return G_SOURCE_REMOVE;
}

// Compute and arm the next-occurrence one-shot timer for a season
// slot. Idempotent; safe to call when the slot is already armed.
static void arm_season_slot(season_slot_t* slot) {
    cancel_source(&slot->timer);

    time_t now;
    time(&now);

    time_t next = 0;
    int rc = seasonal_next(now, slot->kind, &next);
    if (rc != 0) {
        LOG_WARN("seasonal_next('%s') failed (rc=%d); slot disarmed",
                 slot->event_id, rc);
        return;
    }

    if (next <= now) {
        LOG_WARN("seasonal_next('%s') returned past instant %lld <= now %lld; not arming",
                 slot->event_id, (long long)next, (long long)now);
        return;
    }

    // Next seasonal event is ~91 days at most = ~7.9M seconds. Within
    // guint range on every supported platform.
    guint delay = (guint)(next - now);
    GSource* src = g_timeout_source_new_seconds(delay);
    if (!src) {
        LOG_WARN("season '%s': failed to allocate timer source", slot->event_id);
        return;
    }
    season_fire_args_t* args = g_new(season_fire_args_t, 1);
    args->slot = slot;
    g_source_set_callback(src, season_fire_callback, args, g_free);
    g_source_attach(src, NULL);
    slot->timer = src;

    LOG("season '%s': armed for UTC %lld (in %u s, ~%.1f days)",
        slot->event_id, (long long)next, delay, (double)delay / 86400.0);
}

// ---------------------------------------------------------------------
// Daily recompute pipeline
// ---------------------------------------------------------------------

static gboolean midnight_timer_callback(gpointer user_data);

// One full daily recompute: read lat/lon, compute solar events at every
// zenith we care about + lunar daily events, arm each slot's timer.
static int recompute_today(void) {
    double lat = ACAP_DEVICE_Latitude();
    double lon = ACAP_DEVICE_Longitude();

    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        LOG_WARN("Invalid camera geolocation lat=%f lon=%f; daily events disarmed",
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

    LOG("Recomputing daily events for lat=%f lon=%f date=%04d-%02d-%02d",
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

    // Lunar daily events. One call covers all four lunar slots
    // (rise/set/transit/anti-transit). Note: lunar_compute_daily takes
    // a UTC date, and like the solar block above we feed it the local
    // calendar date — this matches the per-day windowing the existing
    // host fixtures validate against.
    lunar_events_t l = {0};
    int lrc = lunar_compute_daily(lat, lon, year, month, day, &l);
    if (lrc != 0) {
        LOG_WARN("lunar_compute_daily failed (rc=%d); disarming lunar daily slots",
                 lrc);
        // lunar_compute_daily on -1 sets all fields to LUNAR_NO_EVENT
        // already, but be explicit so a future API change doesn't
        // silently leak stale handles.
        l.moonrise = l.moonset = l.lunar_transit = l.lunar_anti_transit
            = LUNAR_NO_EVENT;
    } else {
        LOG("lunar moonrise=%lld moonset=%lld transit=%lld anti=%lld",
            (long long)l.moonrise, (long long)l.moonset,
            (long long)l.lunar_transit, (long long)l.lunar_anti_transit);
    }

    // Arm each slot from the matching source.
    for (size_t i = 0; i < EVENT_SLOT_COUNT; i++) {
        event_slot_t* slot = &event_slots[i];
        const solar_events_t* src = &e_std;
        if (slot_is_lunar(slot)) {
            // Lunar slots get their time from `l`; the solar argument
            // is irrelevant but pass e_std for consistency.
            arm_event_slot(slot, time_for_slot(slot, &e_std, &l), now);
            continue;
        }
        if (slot->zenith_deg == SOLAR_ZENITH_CIVIL_TWILIGHT)             src = &e_civil;
        else if (slot->zenith_deg == SOLAR_ZENITH_NAUTICAL_TWILIGHT)     src = &e_naut;
        else if (slot->zenith_deg == SOLAR_ZENITH_ASTRONOMICAL_TWILIGHT) src = &e_astro;
        arm_event_slot(slot, time_for_slot(slot, src, NULL), now);
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
    // Phase + season slots are independent of lat/lon and the daily
    // recompute cadence; arm each one for its next occurrence at boot.
    // They re-arm themselves in their own fire callbacks.
    for (size_t i = 0; i < PHASE_SLOT_COUNT; i++)
        arm_phase_slot(&phase_slots[i]);
    for (size_t i = 0; i < SEASON_SLOT_COUNT; i++)
        arm_season_slot(&season_slots[i]);
    return rc;
}

int timers_recompute_now(void) {
    // Phase and season slots are observer-independent — geolocation
    // changes do not affect them. Touching only the daily slots avoids
    // unnecessary churn (cancel + lunar_next_phase / seasonal_next
    // recompute) on every location update.
    return recompute_today();
}

void timers_cleanup(void) {
    cancel_source(&midnight_timer);
    for (size_t i = 0; i < EVENT_SLOT_COUNT; i++)
        cancel_source(&event_slots[i].timer);
    for (size_t i = 0; i < PHASE_SLOT_COUNT; i++)
        cancel_source(&phase_slots[i].timer);
    for (size_t i = 0; i < SEASON_SLOT_COUNT; i++)
        cancel_source(&season_slots[i].timer);
}
