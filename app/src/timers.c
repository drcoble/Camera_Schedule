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
#include "anchors.h"
#include "calendar.h"
#include "log.h"
#include "status.h"
#include "astro/solar.h"
#include "astro/lunar.h"
#include "astro/seasonal.h"
#include "acap/ACAP.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
// Recompute coalescing (FR-10.3) and per-pass counters (FR-13.3)
// ---------------------------------------------------------------------
//
// HTTP handlers run on the FastCGI worker thread (see acap/ACAP.c
// fastcgi_thread_func), the GLib main loop runs the midnight + per-event
// callbacks. Both can call timers_recompute_now / recompute_today
// concurrently. We use a g_atomic_int "in_flight" gate plus a queued
// flag: at most one recompute runs at any instant; a concurrent caller
// sets `queued` and returns TIMERS_RECOMPUTE_QUEUED, and the running
// pass loops back through recompute_today once more after it finishes.
//
// `pending_trigger` is what the next recompute should record itself as.
// The currently-running pass picks it up under the gate; a queued
// caller that arrives mid-pass overwrites it (the most recent intent
// wins, which is what the operator expects).

static volatile gint g_recompute_in_flight = 0;
static volatile gint g_recompute_queued    = 0;
static recompute_trigger_t g_pending_trigger = RECOMPUTE_TRIGGER_BOOT;
static GMutex g_pending_lock;
static int    g_pending_lock_init = 0;

// Per-recompute counters. Only the recompute thread touches these
// while it holds the in-flight gate; readers consult the status ring
// after the pass completes.
static int g_pass_anchors_evaluated = 0;
static int g_pass_events_armed      = 0;
static int g_pass_skipped_polar     = 0;
static int g_pass_skipped_disabled  = 0;
static int g_pass_skipped_past      = 0;
static int g_pass_errors            = 0;

// ---------------------------------------------------------------------
// Anchor slot configuration (M6 — operator-defined anchors)
// ---------------------------------------------------------------------
//
// Anchor slots are *dynamic*: built from anchors_get_by_index() at
// every recompute. We do not maintain a stable handle table because
// FR-9.3 requires a config change to cancel armed timers anyway, so a
// simple "tear down and rebuild" pattern is correct and avoids the
// plumbing of an indexable table that survives across mutations.
//
// Each anchor armed during a recompute owns up to two GSource handles
// (start + end edge for paired / stateful-offset anchors; one for
// pulse anchors). They live until the next recompute or until the slot
// list is freed in timers_cleanup.
typedef struct {
    char     event_id[64];   // copy of anchor.id (avoids dangling ptr)
    GSource* start_timer;
    GSource* end_timer;
} anchor_slot_t;

static anchor_slot_t* anchor_slots      = NULL;
static size_t         anchor_slot_count = 0;

// Forward decl — cancel_source is defined below in the Helpers section
// but anchor_slots_cleanup needs it.
static void cancel_source(GSource** src);

static void anchor_slots_cleanup(void) {
    if (!anchor_slots) return;
    for (size_t i = 0; i < anchor_slot_count; i++) {
        cancel_source(&anchor_slots[i].start_timer);
        cancel_source(&anchor_slots[i].end_timer);
    }
    free(anchor_slots);
    anchor_slots = NULL;
    anchor_slot_count = 0;
}

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
    g_pass_anchors_evaluated++;

    // FR-11.7 / DL-18: a disabled schedule keeps its AXEvent topic
    // declared but the firing-path arm is suppressed.
    if (!anchors_is_enabled(slot->event_id)) {
        LOG("event '%s': disabled per schedule_enabled.json; not arming",
            slot->event_id);
        g_pass_skipped_disabled++;
        return;
    }

    // SOLAR_NO_EVENT and LUNAR_NO_EVENT are both defined as (time_t)-1,
    // so a single sentinel check covers both daily-event domains.
    if (when == SOLAR_NO_EVENT) {
        LOG("event '%s': no event today (polar/extreme zenith or no horizon crossing), skipping",
            slot->event_id);
        g_pass_skipped_polar++;
        return;
    }
    if (when <= now) {
        LOG("event '%s': already passed today (%lld <= %lld), skipping",
            slot->event_id, (long long)when, (long long)now);
        g_pass_skipped_past++;
        return;
    }

    guint delay = (guint)(when - now);
    GSource* src = g_timeout_source_new_seconds(delay);
    if (!src) {
        LOG_WARN("event '%s': failed to allocate timer source", slot->event_id);
        g_pass_errors++;
        return;
    }
    fire_args_t* args = g_new(fire_args_t, 1);
    args->event_id = slot->event_id;
    g_source_set_callback(src, event_fire_callback, args, g_free);
    g_source_attach(src, NULL);
    slot->timer = src;
    g_pass_events_armed++;

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
    g_pass_anchors_evaluated++;

    if (!anchors_is_enabled(slot->event_id)) {
        LOG("phase '%s': disabled per schedule_enabled.json; not arming",
            slot->event_id);
        g_pass_skipped_disabled++;
        return;
    }

    time_t now;
    time(&now);

    time_t next = 0;
    int rc = lunar_next_phase(now, slot->kind, &next);
    if (rc != 0) {
        LOG_WARN("lunar_next_phase('%s') failed (rc=%d); slot disarmed",
                 slot->event_id, rc);
        g_pass_errors++;
        return;
    }

    // Defensive: the lunar_next_phase contract guarantees the result
    // is strictly after `now`, but if numerical drift ever produced a
    // past value we'd loop-fire forever. Refuse to arm in that case
    // and emit a warning so the issue is visible in syslog.
    if (next <= now) {
        LOG_WARN("lunar_next_phase('%s') returned past instant %lld <= now %lld; not arming",
                 slot->event_id, (long long)next, (long long)now);
        g_pass_skipped_past++;
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
        g_pass_errors++;
        return;
    }
    phase_fire_args_t* args = g_new(phase_fire_args_t, 1);
    args->slot = slot;
    g_source_set_callback(src, phase_fire_callback, args, g_free);
    g_source_attach(src, NULL);
    slot->timer = src;
    g_pass_events_armed++;

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
    g_pass_anchors_evaluated++;

    if (!anchors_is_enabled(slot->event_id)) {
        LOG("season '%s': disabled per schedule_enabled.json; not arming",
            slot->event_id);
        g_pass_skipped_disabled++;
        return;
    }

    time_t now;
    time(&now);

    time_t next = 0;
    int rc = seasonal_next(now, slot->kind, &next);
    if (rc != 0) {
        LOG_WARN("seasonal_next('%s') failed (rc=%d); slot disarmed",
                 slot->event_id, rc);
        g_pass_errors++;
        return;
    }

    if (next <= now) {
        LOG_WARN("seasonal_next('%s') returned past instant %lld <= now %lld; not arming",
                 slot->event_id, (long long)next, (long long)now);
        g_pass_skipped_past++;
        return;
    }

    // Next seasonal event is ~91 days at most = ~7.9M seconds. Within
    // guint range on every supported platform.
    guint delay = (guint)(next - now);
    GSource* src = g_timeout_source_new_seconds(delay);
    if (!src) {
        LOG_WARN("season '%s': failed to allocate timer source", slot->event_id);
        g_pass_errors++;
        return;
    }
    season_fire_args_t* args = g_new(season_fire_args_t, 1);
    args->slot = slot;
    g_source_set_callback(src, season_fire_callback, args, g_free);
    g_source_attach(src, NULL);
    slot->timer = src;
    g_pass_events_armed++;

    LOG("season '%s': armed for UTC %lld (in %u s, ~%.1f days)",
        slot->event_id, (long long)next, delay, (double)delay / 86400.0);
}

// ---------------------------------------------------------------------
// Anchor scheduler — pulse, paired, and threshold helpers
// ---------------------------------------------------------------------
//
// Threshold anchors. Per the M6 contract (M6_API_CONTRACT.md §2.2 / OQ-13)
// the qualifying-day metric is sampled at *local solar noon* of each
// day in the look-ahead window, and on a satisfying day the pulse
// fires at *local solar midnight* of that day. For M6 we use *local
// civil midnight / civil noon* as a simplification — solar midnight at
// non-polar latitudes is within ~30 min of civil midnight (the
// equation-of-time + longitude offset), which is well below the
// resolution at which a "pulse on bright nights" trigger needs to be
// timed. Tightening this to the astro/solar.c primitive can land in
// M7 without breaking the wire schema.

typedef struct {
    char  event_id[64];
    int   stateful;
} fire_state_args_t;

static gboolean fire_state_true_cb(gpointer user_data) {
    fire_state_args_t* a = (fire_state_args_t*)user_data;
    if (anchors_is_enabled(a->event_id)) {
        LOG("Firing anchor '%s' state=true", a->event_id);
        ACAP_EVENTS_Fire_State(a->event_id, 1);
    }
    return G_SOURCE_REMOVE;
}

static gboolean fire_state_false_cb(gpointer user_data) {
    fire_state_args_t* a = (fire_state_args_t*)user_data;
    if (anchors_is_enabled(a->event_id)) {
        LOG("Firing anchor '%s' state=false", a->event_id);
        ACAP_EVENTS_Fire_State(a->event_id, 0);
    }
    return G_SOURCE_REMOVE;
}

static gboolean fire_pulse_cb(gpointer user_data) {
    fire_state_args_t* a = (fire_state_args_t*)user_data;
    if (anchors_is_enabled(a->event_id)) {
        LOG("Firing anchor '%s' (pulse)", a->event_id);
        ACAP_EVENTS_Fire(a->event_id);
    }
    return G_SOURCE_REMOVE;
}

// Allocate-and-arm a one-shot. delay_seconds <= 0 ⇒ skip.
static GSource* arm_one_shot(time_t when, time_t now,
                             GSourceFunc cb, fire_state_args_t* args) {
    if (when <= now) return NULL;
    guint delay = (guint)(when - now);
    GSource* s = g_timeout_source_new_seconds(delay);
    if (!s) return NULL;
    g_source_set_callback(s, cb, args, g_free);
    g_source_attach(s, NULL);
    return s;
}

// Arm a single operator anchor for "today." `local_day_anchor` is the
// per-recompute reference instant (typically `now` or a recent past
// time clamped to today's midnight).
static void arm_operator_anchor(anchor_slot_t* slot,
                                const anchor_t* a,
                                time_t now,
                                time_t local_day_anchor) {
    g_pass_anchors_evaluated++;
    if (!anchors_is_enabled(a->id)) {
        LOG("anchor '%s': disabled; not arming", a->id);
        g_pass_skipped_disabled++;
        return;
    }

    if (a->kind == ANCHOR_KIND_OFFSET) {
        time_t base = 0;
        if (anchors_resolve_source(a->event_source, local_day_anchor, &base) != 0) {
            LOG_WARN("anchor '%s': source '%s' unresolved; skipping",
                     a->id, a->event_source);
            g_pass_errors++;
            return;
        }
        if (base == SOLAR_NO_EVENT) {
            LOG("anchor '%s': source '%s' has no event today",
                a->id, a->event_source);
            g_pass_skipped_polar++;
            return;
        }
        time_t when = base + (time_t)(a->offset_minutes * 60);
        if (a->duration_minutes > 0) {
            time_t end = when + (time_t)(a->duration_minutes * 60);
            fire_state_args_t* sa = g_new(fire_state_args_t, 1);
            snprintf(sa->event_id, sizeof sa->event_id, "%s", a->id);
            sa->stateful = 1;
            slot->start_timer = arm_one_shot(when, now, fire_state_true_cb, sa);
            if (!slot->start_timer) g_free(sa);

            fire_state_args_t* ea = g_new(fire_state_args_t, 1);
            snprintf(ea->event_id, sizeof ea->event_id, "%s", a->id);
            ea->stateful = 1;
            slot->end_timer = arm_one_shot(end, now, fire_state_false_cb, ea);
            if (!slot->end_timer) g_free(ea);
        } else {
            fire_state_args_t* pa = g_new(fire_state_args_t, 1);
            snprintf(pa->event_id, sizeof pa->event_id, "%s", a->id);
            pa->stateful = 0;
            slot->start_timer = arm_one_shot(when, now, fire_pulse_cb, pa);
            if (!slot->start_timer) g_free(pa);
        }
        if (slot->start_timer) g_pass_events_armed++;
        else                   g_pass_skipped_past++;
        return;
    }

    if (a->kind == ANCHOR_KIND_PAIRED) {
        time_t s_base = 0, e_base = 0;
        if (anchors_resolve_source(a->start_event, local_day_anchor, &s_base) != 0 ||
            anchors_resolve_source(a->end_event,   local_day_anchor, &e_base) != 0) {
            LOG_WARN("anchor '%s': paired source unresolved; skipping", a->id);
            g_pass_errors++;
            return;
        }
        if (s_base == SOLAR_NO_EVENT || e_base == SOLAR_NO_EVENT) {
            LOG("anchor '%s': paired source missing today; skipping", a->id);
            g_pass_skipped_polar++;
            return;
        }
        time_t s_when = s_base + (time_t)(a->start_offset_minutes * 60);
        time_t e_when = e_base + (time_t)(a->end_offset_minutes * 60);
        if (e_when <= s_when) e_when += 86400;  // crosses local midnight

        fire_state_args_t* sa = g_new(fire_state_args_t, 1);
        snprintf(sa->event_id, sizeof sa->event_id, "%s", a->id);
        sa->stateful = 1;
        slot->start_timer = arm_one_shot(s_when, now, fire_state_true_cb, sa);
        if (!slot->start_timer) g_free(sa);

        fire_state_args_t* ea = g_new(fire_state_args_t, 1);
        snprintf(ea->event_id, sizeof ea->event_id, "%s", a->id);
        ea->stateful = 1;
        slot->end_timer = arm_one_shot(e_when, now, fire_state_false_cb, ea);
        if (!slot->end_timer) g_free(ea);
        if (slot->start_timer) g_pass_events_armed++;
        else                   g_pass_skipped_past++;
        return;
    }

    if (a->kind == ANCHOR_KIND_THRESHOLD) {
        // M6 scope: evaluate today only. Use local civil midnight as a
        // proxy for local solar midnight (see header comment above).
        struct tm tm_local;
        localtime_r(&local_day_anchor, &tm_local);
        tm_local.tm_hour = 12; tm_local.tm_min = 0; tm_local.tm_sec = 0;
        tm_local.tm_isdst = -1;
        time_t noon_today = mktime(&tm_local);

        double frac = lunar_illumination(noon_today);
        if (frac < 0.0) {
            LOG_WARN("anchor '%s': lunar_illumination failed", a->id);
            g_pass_errors++;
            return;
        }
        int satisfied = 0;
        switch (a->op) {
            case ANCHOR_OP_GE: satisfied = (frac >= a->value); break;
            case ANCHOR_OP_LE: satisfied = (frac <= a->value); break;
            case ANCHOR_OP_GT: satisfied = (frac >  a->value); break;
            case ANCHOR_OP_LT: satisfied = (frac <  a->value); break;
        }
        if (!satisfied) {
            LOG("anchor '%s': threshold not met today (frac=%.3f)",
                a->id, frac);
            g_pass_skipped_disabled++;  // semantic: condition unsatisfied
            return;
        }
        // Fire instant: civil midnight of today (proxy for solar mid).
        struct tm tm_mid;
        localtime_r(&local_day_anchor, &tm_mid);
        tm_mid.tm_hour = 0; tm_mid.tm_min = 0; tm_mid.tm_sec = 0;
        tm_mid.tm_isdst = -1;
        time_t fire_at = mktime(&tm_mid);

        fire_state_args_t* pa = g_new(fire_state_args_t, 1);
        snprintf(pa->event_id, sizeof pa->event_id, "%s", a->id);
        pa->stateful = 0;
        slot->start_timer = arm_one_shot(fire_at, now, fire_pulse_cb, pa);
        if (!slot->start_timer) g_free(pa);
        if (slot->start_timer) g_pass_events_armed++;
        else                   g_pass_skipped_past++;
        return;
    }
}

// Tear down and rebuild every operator anchor's slot for today.
static void recompute_anchors(time_t now) {
    anchor_slots_cleanup();

    size_t total = anchors_count();
    if (total == 0) return;

    anchor_slots = calloc(total, sizeof(anchor_slot_t));
    if (!anchor_slots) {
        LOG_WARN("recompute_anchors: calloc failed");
        return;
    }

    for (size_t i = 0; i < total; i++) {
        anchor_t a;
        if (anchors_get_by_index(i, &a) != 0) continue;
        if (a.built_in) continue;  // built-ins are armed by event_slots/etc.

        anchor_slot_t* slot = &anchor_slots[anchor_slot_count];
        snprintf(slot->event_id, sizeof slot->event_id, "%s", a.id);
        arm_operator_anchor(slot, &a, now, now);
        anchor_slot_count++;
    }
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

    // Operator-defined anchors (M6). Tear-down and rebuild because the
    // anchor list itself can have changed since the last recompute.
    recompute_anchors(now);
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
        g_pass_errors++;
        return;
    }
    g_source_set_callback(src, midnight_timer_callback, NULL, NULL);
    g_source_attach(src, NULL);
    midnight_timer = src;

    // Surface the next-recompute schedule for /status. The midnight
    // wake-up is the always-armed daily reference; the per-slot
    // arming above is faster but not single-pointed.
    status_set_next(now + delay, RECOMPUTE_TRIGGER_MIDNIGHT);

    LOG("Midnight recompute armed for %d s from now", delay);
}

static gboolean midnight_timer_callback(gpointer user_data) {
    (void)user_data;
    LOG("Local midnight reached; recomputing daily events");
    (void)timers_recompute_now(RECOMPUTE_TRIGGER_MIDNIGHT);
    arm_midnight_timer();
    return G_SOURCE_REMOVE;
}

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

// Reset per-pass counters before a recompute. Called only by the holder
// of g_recompute_in_flight.
static void reset_pass_counters(void) {
    g_pass_anchors_evaluated = 0;
    g_pass_events_armed      = 0;
    g_pass_skipped_polar     = 0;
    g_pass_skipped_disabled  = 0;
    g_pass_skipped_past      = 0;
    g_pass_errors            = 0;
}

// Run one recompute pass with full instrumentation: timestamp it,
// reset counters, run recompute_today(), then push a status_record.
static int run_one_pass(recompute_trigger_t trigger) {
    reset_pass_counters();

    GTimer* t = g_timer_new();
    time_t  started_utc = time(NULL);
    int rc = recompute_today();
    gulong micros = 0;
    double seconds = g_timer_elapsed(t, &micros);
    g_timer_destroy(t);

    if (rc != 0) g_pass_errors++;

    recompute_summary_t s = {
        .started_at_utc    = started_utc,
        .trigger           = trigger,
        .elapsed_ms        = (int)(seconds * 1000.0 + 0.5),
        .anchors_evaluated = g_pass_anchors_evaluated,
        .events_armed      = g_pass_events_armed,
        .skipped_polar     = g_pass_skipped_polar,
        .skipped_disabled  = g_pass_skipped_disabled,
        .skipped_past      = g_pass_skipped_past,
        .errors            = g_pass_errors
    };
    status_record(&s);

    // FR-10.4 INFO summary. Co-exists with the structured ring entry
    // per contract §3 — journal diagnostics still want the human line.
    LOG("Recompute summary: trigger=%s elapsed_ms=%d anchors=%d armed=%d "
        "skipped_polar=%d skipped_disabled=%d skipped_past=%d errors=%d",
        status_trigger_str(trigger), s.elapsed_ms, s.anchors_evaluated,
        s.events_armed, s.skipped_polar, s.skipped_disabled,
        s.skipped_past, s.errors);

    return rc;
}

int timers_init(void) {
    if (!g_pending_lock_init) {
        g_mutex_init(&g_pending_lock);
        g_pending_lock_init = 1;
    }

    g_atomic_int_set(&g_recompute_in_flight, 1);
    int rc = run_one_pass(RECOMPUTE_TRIGGER_BOOT);
    g_atomic_int_set(&g_recompute_in_flight, 0);

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

int timers_recompute_now(recompute_trigger_t trigger) {
    if (!g_pending_lock_init) {
        // Defensive: callers from anchors.c can land here before
        // timers_init() during boot's seed phase. Initialize the
        // mutex on first use; subsequent calls are no-ops.
        g_mutex_init(&g_pending_lock);
        g_pending_lock_init = 1;
    }

    // Try to acquire the in-flight gate. If another thread is already
    // recomputing, mark the pass queued and return immediately.
    if (!g_atomic_int_compare_and_exchange(&g_recompute_in_flight, 0, 1)) {
        g_mutex_lock(&g_pending_lock);
        g_pending_trigger = trigger;
        g_mutex_unlock(&g_pending_lock);
        g_atomic_int_set(&g_recompute_queued, 1);
        LOG("Recompute trigger=%s queued (one already in flight)",
            status_trigger_str(trigger));
        return TIMERS_RECOMPUTE_QUEUED;
    }

    int rc = run_one_pass(trigger);

    // Drain the queue. If a second trigger arrived during the pass,
    // re-run with that trigger. Loop guards against starvation:
    // bounded to 4 follow-ups so a stampede can't pin the thread.
    int loops = 0;
    while (g_atomic_int_compare_and_exchange(&g_recompute_queued, 1, 0) &&
           loops < 4) {
        g_mutex_lock(&g_pending_lock);
        recompute_trigger_t pending = g_pending_trigger;
        g_mutex_unlock(&g_pending_lock);
        rc = run_one_pass(pending);
        loops++;
    }

    g_atomic_int_set(&g_recompute_in_flight, 0);
    return rc == 0 ? TIMERS_RECOMPUTE_OK : TIMERS_RECOMPUTE_ERROR;
}

void timers_cleanup(void) {
    cancel_source(&midnight_timer);
    for (size_t i = 0; i < EVENT_SLOT_COUNT; i++)
        cancel_source(&event_slots[i].timer);
    for (size_t i = 0; i < PHASE_SLOT_COUNT; i++)
        cancel_source(&phase_slots[i].timer);
    for (size_t i = 0; i < SEASON_SLOT_COUNT; i++)
        cancel_source(&season_slots[i].timer);
    anchor_slots_cleanup();
}
