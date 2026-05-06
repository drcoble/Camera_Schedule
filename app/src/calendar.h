// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// User calendar entries — public surface (FR-6).
//
// Each calendar entry is a named, operator-curated date or date set
// that can be referenced as an anchor's `event_source` (FR-6.4) and
// that registers its own AXEvent topic (FR-8.1) so operators can wire
// camera Action Rules directly to it without first wrapping it in an
// anchor.
//
// The app does NOT ship a holiday database (FR-6.3). All entries are
// operator input.
//
// Like anchors.h, this module's public surface is pure C with no
// FastCGI / cJSON dependency; the on-disk encoding (cJSON) is bound
// at the .c-file level.

#ifndef CAMERA_SCHEDULE_CALENDAR_H
#define CAMERA_SCHEDULE_CALENDAR_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------
// Limits and naming rules
// ---------------------------------------------------------------------

// Soft cap on calendar entries. Mirrors the anchor cap (DL-14) — each
// calendar entry consumes one AXEvent topic when registered, so the
// fleet-rendering arithmetic is the same. UI rendering, not a hard
// technical limit.
#define CALENDAR_OPERATOR_MAX 64

// IDs follow the same regex / length rule as anchor IDs
// (^[a-z0-9_]{1,32}$) and live in the same global ID namespace —
// collisions with built-in anchor IDs, operator anchor IDs, or other
// calendar IDs are rejected (HTTP 409). See M6_API_CONTRACT.md.
#define CALENDAR_ID_MAX   32
#define CALENDAR_NAME_MAX 64

// Free-form notes field on each entry (FR-6.2). Not used in scheduling;
// surfaced in the configuration UI only.
#define CALENDAR_NOTES_MAX 256

// ---------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------

typedef enum {
    CALENDAR_KIND_SINGLE_DATE = 0,  // one specific Gregorian date
    CALENDAR_KIND_DATE_RANGE  = 1,  // inclusive [start_date, end_date]
    CALENDAR_KIND_ANNUAL      = 2   // recurring every year on (month, day)
} calendar_kind_t;

// Time-of-day mode for an entry's fire instant.
//
// `CALENDAR_TIME_ALL_DAY`  — interval-style: the entry's topic is
//                            *stateful*, high for the entire local
//                            civil day (00:00 → 24:00 local).
// `CALENDAR_TIME_SPECIFIC` — pulse-style: the entry's topic fires
//                            once at `time_of_day_seconds` past local
//                            midnight on the matching date.
typedef enum {
    CALENDAR_TIME_ALL_DAY  = 0,
    CALENDAR_TIME_SPECIFIC = 1
} calendar_time_mode_t;

// Compact representation of a Gregorian date. `year` is ignored when
// the parent `kind` is CALENDAR_KIND_ANNUAL (only month/day matter).
typedef struct {
    int year;   // Gregorian, e.g. 2026 — ignored for annual entries
    int month;  // 1..12
    int day;    // 1..31, validated against month + leap-year
} calendar_date_t;

// One calendar entry.
typedef struct {
    char id[CALENDAR_ID_MAX + 1];
    char name[CALENDAR_NAME_MAX + 1];

    calendar_kind_t      kind;
    calendar_time_mode_t time_mode;

    // For CALENDAR_TIME_SPECIFIC: seconds past local civil midnight,
    // [0, 86399]. Ignored for CALENDAR_TIME_ALL_DAY.
    int time_of_day_seconds;

    // Date payload, interpreted per `kind`:
    //   CALENDAR_KIND_SINGLE_DATE → `start_date` only; `end_date` ignored.
    //   CALENDAR_KIND_DATE_RANGE  → both used; `end_date` >= `start_date`.
    //   CALENDAR_KIND_ANNUAL      → `start_date.{month,day}` only;
    //                               year and `end_date` ignored.
    calendar_date_t start_date;
    calendar_date_t end_date;

    // Free-form, optional. Not used in scheduling.
    char notes[CALENDAR_NOTES_MAX + 1];

    // FR-11.7 enable-state mirror. The authoritative store is
    // `localdata/schedule_enabled.json`; this field is a snapshot
    // populated on read by anchors_init / calendar_init from that
    // store. Toggling goes through anchors_set_enabled() (the entry
    // point is shared across anchors and calendar entries because the
    // gate lives on the firing path, not on the entity kind).
    int enabled;
} calendar_entry_t;

// ---------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------

// Initialize the calendar module: load `localdata/calendar.json` per
// FR-12.1, apply the FR-11.7 enable store, and prepare the in-memory
// list. Empty / missing file ⇒ no entries; malformed file ⇒ rename to
// `calendar.json.broken-<unix-ts>`, log at ERR, start empty (FR-12.4).
//
// MUST be called after ACAP() and before timers_init() registers
// per-entry topics. Returns 0 on success, -1 on unrecoverable failure.
int calendar_init(void);

// Tear down the in-memory list. Idempotent; safe in SIGTERM.
void calendar_cleanup(void);

// ---------------------------------------------------------------------
// Read access
// ---------------------------------------------------------------------

// Total count of entries.
size_t calendar_count(void);

// Copy the i-th entry (0-indexed, in the order they appear in
// localdata/calendar.json) into `*out`. Returns 0 on success, -1 on
// out-of-range index or NULL `out`.
int calendar_get_by_index(size_t index, calendar_entry_t* out);

// Copy the entry with the given `id` into `*out`. Returns 0 on success,
// -1 if no such entry or NULL pointer. Case-sensitive match.
int calendar_get_by_id(const char* id, calendar_entry_t* out);

// Find the next UTC instant on or after `after` at which the calendar
// entry "fires" — for SPECIFIC time_mode, the matching local date's
// `time_of_day_seconds`; for ALL_DAY, the local 00:00 of the matching
// date.
//
// `match_end_out` (optional, may be NULL) receives the corresponding
// state-low time for ALL_DAY entries (the next local 24:00 after the
// fire) or for any future stateful semantics. For SPECIFIC entries,
// `match_end_out` is set to (time_t)0.
//
// Returns 0 on success, with `*out` written. Returns 0 with `*out =
// (time_t)-1` if no occurrence exists in the look-ahead window
// (single-date entries in the past are the canonical case). Returns
// -1 on bad input.
int calendar_next_occurrence(const char* id,
                             time_t      after,
                             time_t*     out,
                             time_t*     match_end_out);

// True (non-zero) iff the calendar entry is "active" at the given UTC
// instant — used by anchor paired/threshold lookups and by
// events_today's `not_firing_today` synthesis. For SPECIFIC entries,
// active = the instant equals the fire time (within ±1 s); for ALL_DAY,
// active = `when` is inside the local civil day(s) covered by the
// entry's date payload.
//
// Returns 0 (not active) or 1 (active). Returns -1 on unknown id.
int calendar_is_active_at(const char* id, time_t when);

// ---------------------------------------------------------------------
// Mutation (admin-write per DL-13)
// ---------------------------------------------------------------------
//
// Mirrors the anchors_* mutator contract: atomic JSON write, all-or-
// nothing validation, AXEvent reconciliation per FR-8.5, recompute
// trigger per FR-10.1. On any failure, in-memory list, on-disk file,
// and AXEvent declaration table are rolled back.

#define CALENDAR_OK              0
#define CALENDAR_ERR_NOT_FOUND  -1
#define CALENDAR_ERR_INVALID    -3
#define CALENDAR_ERR_DUPLICATE  -4
#define CALENDAR_ERR_FULL       -5
#define CALENDAR_ERR_PERSIST    -7
#define CALENDAR_ERR_REGISTER   -8
#define CALENDAR_ERR_INTERNAL   -9

// (Note: error code numbers match the anchors_* corresponding codes
// for the same condition, so a shared HTTP-status mapping table can
// be used.)

// Create a new calendar entry. `in->enabled` is ignored on input —
// new entries default to enabled per FR-11.7.
int calendar_create(const calendar_entry_t* in);

// Replace an existing entry, keyed by `in->id`. Full-replacement model.
// Reconciliation behaves the same as anchors_update: id-stable name-
// only changes do an AXEvent re-declare; other field changes cancel
// armed timers and re-arm.
int calendar_update(const calendar_entry_t* in);

// Delete a calendar entry by id. If any operator-defined anchor
// currently references the entry as `event_source` / `start_event` /
// `end_event`, the delete still succeeds — the dangling reference is
// detected at recompute time and the dependent anchor is skipped on
// arm with a WARN log. (Referential-integrity gating is left to the
// UI per FR-11.5; see anchors.h for the matching note.)
int calendar_delete(const char* id);

// Atomic bulk replacement (POST /calendar "replace all" mode and
// future M7 import). Same all-or-nothing validation as
// anchors_replace_all.
int calendar_replace_all(const calendar_entry_t* entries, size_t count);

#ifdef __cplusplus
}
#endif

#endif  // CAMERA_SCHEDULE_CALENDAR_H
