// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Schedule anchors — public surface (FR-7).
//
// An anchor is the user-facing primitive that ties a computed astronomical
// or calendar event to a registered AXEvent topic. The camera's Action
// Rules UI sees one topic per anchor and operators bind their existing
// rules (recording, MQTT, day/night switch, …) to the topic. See
// `requirements/07-schedule-anchors.md` and DL-05 for background.
//
// This module is the M6 addition to the existing solar / lunar / seasonal
// scheduler in timers.c (see timers.h for the three pre-existing
// patterns — daily, phase, season). Anchors layer on top: each anchor
// computes its fire time(s) by looking up its `event_source` and applying
// the kind-specific arithmetic below, then arms a GLib timer through the
// same machinery the built-in slots use.
//
// Three anchor kinds are supported:
//
//   * ANCHOR_KIND_OFFSET     — a single source event ± offset_minutes.
//                              Fires as a *pulse* if `duration_minutes`
//                              is unset; *stateful* (high → low) if set.
//   * ANCHOR_KIND_PAIRED     — two source events form an interval.
//                              Always *stateful*: high while inside
//                              the interval, low outside (FR-7.4).
//   * ANCHOR_KIND_THRESHOLD  — pulses on each local civil day in the
//                              look-ahead window where a numeric metric
//                              (v1: moon illumination) satisfies the
//                              configured operator/value (FR-7.7).
//
// Built-in anchors. The 22 events declared in app/settings/events.json
// (10 solar + 8 lunar + 4 seasonal) are surfaced as built-in anchors at
// boot with `built_in=true`, `kind=ANCHOR_KIND_OFFSET`,
// `offset_minutes=0`, `duration_minutes=0`, `enabled=true` (subject to
// the FR-11.7 enable-state store). Built-ins are non-deletable and are
// NOT persisted in localdata/anchors.json — only operator-defined
// anchors live in that file.
//
// Like the astro/* headers and timers.h, this module is pure C with no
// FastCGI / cJSON dependency in its public surface — those are bound at
// the .c-file level. The UI agent and test agent code against this
// header alone.

#ifndef CAMERA_SCHEDULE_ANCHORS_H
#define CAMERA_SCHEDULE_ANCHORS_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------
// Limits and naming rules
// ---------------------------------------------------------------------

// Soft cap on operator-defined anchors. Built-in anchors are NOT counted
// against this cap. See DL-14 / FR-7.3.
#define ANCHORS_OPERATOR_MAX 64

// Maximum length (excluding the NUL terminator) of an anchor `id`.
// 32 chars is a UI-rendering / topic-path-readability cap — there is no
// AXEvent technical limit driving this number. The full topic path
// becomes
//   tnsaxis:CameraApplicationPlatform/camera_schedule/<id>
// per FR-7.6. Operator-defined IDs SHALL match the regex
//   ^[a-z0-9_]{1,32}$
// and SHALL be unique within the union of {built-in anchor IDs,
// operator-defined anchor IDs, calendar-entry IDs} — collisions across
// any of those three are rejected with HTTP 409 (see M6_API_CONTRACT.md).
#define ANCHORS_ID_MAX 32

// Maximum length (excluding the NUL terminator) of an anchor `name`
// (the human-readable label that becomes the AXEvent NiceName). 64 is
// a UI rendering judgment matched to the Axis Action Rules dropdown.
#define ANCHORS_NAME_MAX 64

// Offset arithmetic bounds (FR-7.1). Inclusive on both ends; values
// outside this range are rejected as invalid.
#define ANCHORS_OFFSET_MIN_MINUTES (-1440)
#define ANCHORS_OFFSET_MAX_MINUTES ( 1440)

// Maximum duration on a stateful offset anchor. 1440 min = 24 h, which
// is the longest duration that fits the daily-recompute cadence
// without spanning more than one local civil day. Paired anchors have
// no explicit duration cap — they are bounded by the gap between their
// two source events.
#define ANCHORS_DURATION_MAX_MINUTES 1440

// ---------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------

typedef enum {
    ANCHOR_KIND_OFFSET    = 0,  // single event_source ± offset
    ANCHOR_KIND_PAIRED    = 1,  // two event_source references → interval
    ANCHOR_KIND_THRESHOLD = 2   // numeric daily threshold (v1: illumination)
} anchor_kind_t;

// Numeric metric vocabulary for ANCHOR_KIND_THRESHOLD. v1 supports
// moon illumination only; new metrics are added as enum values without
// breaking the JSON schema (the schema validator rejects unknown
// strings, see M6_API_CONTRACT.md).
typedef enum {
    ANCHOR_METRIC_MOON_ILLUMINATION = 0
} anchor_metric_t;

// Comparison operator for ANCHOR_KIND_THRESHOLD. The threshold is
// satisfied on a local civil day iff `metric(noon) <op> value` (or, for
// metrics that are "instant" rather than "daily averaged", `metric` is
// evaluated at local solar noon of the day). v1: moon illumination is
// the synodic phase, which varies slowly enough that any sample within
// the day works; we standardize on local solar noon.
typedef enum {
    ANCHOR_OP_GE = 0,   // metric >= value
    ANCHOR_OP_LE = 1,   // metric <= value
    ANCHOR_OP_GT = 2,   // metric >  value
    ANCHOR_OP_LT = 3    // metric <  value
} anchor_op_t;

// One anchor record. The struct shape matches the JSON-on-disk schema
// in M6_API_CONTRACT.md 1:1 — fields not relevant to a given `kind` are
// expected to carry zero / sentinel values per the per-kind notes
// below. CRUD always copies in/out by value (no caller-owned heap
// references inside the struct besides the inline char arrays).
typedef struct {
    char id[ANCHORS_ID_MAX + 1];
    char name[ANCHORS_NAME_MAX + 1];

    anchor_kind_t kind;
    int           enabled;        // boolean; 0 or 1
    int           built_in;       // boolean; 0 = operator-defined, 1 = built-in

    // ANCHOR_KIND_OFFSET fields ---------------------------------------
    //
    // `event_source`: ID of any built-in anchor (e.g. "sunrise"), any
    // operator-defined anchor, or any calendar entry. Resolution
    // happens at compute time; if the referenced ID disappears (e.g.
    // calendar entry deleted), the anchor logs a WARN and is silently
    // skipped on arm. See FR-8.5 reconciliation.
    //
    // `offset_minutes`: signed integer, [ANCHORS_OFFSET_MIN_MINUTES,
    // ANCHORS_OFFSET_MAX_MINUTES]. Applied as absolute UTC seconds:
    //     fire_time_utc = source_time_utc + offset_minutes * 60
    // Therefore DST transitions are transparent — the offset is in real
    // time, not wall-clock minutes.
    //
    // `duration_minutes`: 0 = pulse anchor (FR-8.3 pulse semantics).
    // > 0 = stateful anchor with the topic high for `duration_minutes`
    // beginning at fire time. Range (0, ANCHORS_DURATION_MAX_MINUTES].
    char event_source[ANCHORS_ID_MAX + 1];
    int  offset_minutes;
    int  duration_minutes;

    // ANCHOR_KIND_PAIRED fields ---------------------------------------
    //
    // For paired anchors the topic is always *stateful* (FR-7.4): high
    // from `start_event + start_offset_minutes` to `end_event +
    // end_offset_minutes`. Pairs that cross local midnight are valid
    // (e.g. "sunset → sunrise next day" → night). The end-event time
    // resolved for the same calendar day is used; if it precedes the
    // start, the next-day occurrence of `end_event` is taken.
    //
    // For non-paired anchors these fields are zero-initialized and
    // ignored.
    char start_event[ANCHORS_ID_MAX + 1];
    int  start_offset_minutes;
    char end_event[ANCHORS_ID_MAX + 1];
    int  end_offset_minutes;

    // ANCHOR_KIND_THRESHOLD fields ------------------------------------
    //
    // For threshold anchors the recompute pipeline evaluates
    // `metric(local_solar_noon_of_day) <op> value` for each local civil
    // day in the look-ahead window (NFR-2 default: 90 days) and arms a
    // pulse on satisfying days. The fire time on a satisfying day is
    // **local solar midnight of that day** by default (matching the
    // existing FR-10.1 midnight cadence); see OQ-13 in M6_API_CONTRACT.md
    // — this is the open question Phase 1 chose a defensible default
    // for. Edits to a threshold anchor always re-evaluate the full
    // look-ahead window.
    //
    // For non-threshold anchors these fields are zero-initialized and
    // ignored.
    anchor_metric_t metric;
    anchor_op_t     op;
    double          value;
} anchor_t;

// ---------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------

// Initialize the anchor module:
//
//   1. Seed the in-memory list with the 22 built-in anchors derived
//      from app/settings/events.json (one ANCHOR_KIND_OFFSET anchor per
//      declared topic, all with offset=0 / duration=0 / enabled=true /
//      built_in=true).
//   2. Load operator-defined anchors from `localdata/anchors.json`
//      (atomic-write/validate per FR-12.1). Missing or empty file ⇒
//      no operator anchors. Malformed file ⇒ rename to
//      `anchors.json.broken-<unix-ts>`, log at ERR, start with no
//      operator anchors (FR-12.4).
//   3. Apply the FR-11.7 enable-state store (`schedule_enabled.json`)
//      to every loaded anchor's `enabled` field — absent keys default
//      to enabled.
//
// MUST be called after ACAP() is initialized (so ACAP_FILE_* sandbox
// paths resolve) and before timers_init() registers per-anchor topics.
//
// Returns 0 on success, -1 on unrecoverable failure (e.g. malformed
// `settings/events.json`). Operator-config load failures are recovered
// from per FR-12.4 and do not propagate as -1.
int anchors_init(void);

// Tear down the module: free the in-memory list and any internal
// caches. Safe to call from the SIGTERM handler. Idempotent.
void anchors_cleanup(void);

// ---------------------------------------------------------------------
// Read access (used by HTTP GET endpoints, by timers.c when arming, and
// by events_today)
// ---------------------------------------------------------------------

// Total count of anchors in the in-memory list (built-in + operator).
size_t anchors_count(void);

// Copy the i-th anchor (0-indexed, in declaration order: built-ins
// first, then operator-defined in the order they appear in
// localdata/anchors.json) into `*out`. Returns 0 on success, -1 on
// out-of-range index or NULL `out`.
int anchors_get_by_index(size_t index, anchor_t* out);

// Copy the anchor whose `id` matches into `*out`. Returns 0 on success,
// -1 if no such anchor exists or `out`/`id` is NULL. The match is
// case-sensitive (IDs are lowercase per the regex above).
int anchors_get_by_id(const char* id, anchor_t* out);

// Resolve an `event_source` reference (or a paired anchor's
// start/end_event) to its fire time on the local civil day containing
// `local_day_anchor` (typically: `time(NULL)` for "today").
//
// Sources are looked up in this order:
//   1. Built-in anchors (solar / lunar / seasonal / daily lunar) —
//      delegates to the existing astro/* primitives.
//   2. Operator-defined anchors — recursively resolves their fire
//      time. Cycles are detected and broken with a WARN log;
//      a cycle returns SOLAR_NO_EVENT and skips the anchor.
//   3. Calendar entries (FR-6) — delegates to calendar.h.
//
// Returns 0 on success and writes the UTC instant into `*out`.
// Returns 0 with `*out = SOLAR_NO_EVENT` (== (time_t)-1) when the
// source legitimately has no event today (e.g. polar moonrise, calendar
// entry that does not match this date). Returns -1 on hard error
// (unknown id, NULL out, malformed source).
int anchors_resolve_source(const char* event_source,
                           time_t       local_day_anchor,
                           time_t*      out);

// ---------------------------------------------------------------------
// Mutation (operator-defined only; built-ins are not modifiable)
// ---------------------------------------------------------------------
//
// All mutators preserve invariants on success and roll back on failure:
//   * persist localdata/anchors.json atomically (FR-12.1)
//   * trigger reconciliation (declare/undeclare/redeclare) per FR-8.5
//   * trigger a recompute (timers_recompute_now equivalent for the
//     affected slots) per FR-10.1
// On failure, the in-memory list, the on-disk file, and the AXEvent
// declaration table all remain in their pre-call state.
//
// Result codes (negative are errors):
//   ANCHORS_OK                 0   success
//   ANCHORS_ERR_NOT_FOUND     -1   no such id
//   ANCHORS_ERR_BUILTIN       -2   attempt to modify/delete a built-in
//   ANCHORS_ERR_INVALID       -3   field out of range, regex mismatch
//   ANCHORS_ERR_DUPLICATE     -4   id already in use
//   ANCHORS_ERR_FULL          -5   operator cap reached
//   ANCHORS_ERR_DEP           -6   referenced event_source not found
//   ANCHORS_ERR_PERSIST       -7   atomic JSON write failed
//   ANCHORS_ERR_REGISTER      -8   AXEvent declare/undeclare failed
//   ANCHORS_ERR_INTERNAL      -9   unexpected (out-of-memory, …)

#define ANCHORS_OK              0
#define ANCHORS_ERR_NOT_FOUND  -1
#define ANCHORS_ERR_BUILTIN    -2
#define ANCHORS_ERR_INVALID    -3
#define ANCHORS_ERR_DUPLICATE  -4
#define ANCHORS_ERR_FULL       -5
#define ANCHORS_ERR_DEP        -6
#define ANCHORS_ERR_PERSIST    -7
#define ANCHORS_ERR_REGISTER   -8
#define ANCHORS_ERR_INTERNAL   -9

// Create a new operator-defined anchor. The caller fills `in->id`,
// `in->name`, `in->kind`, `in->enabled`, and the kind-specific fields.
// `in->built_in` is ignored on input and forced to 0.
int anchors_create(const anchor_t* in);

// Update an existing operator-defined anchor in place, keyed by
// `in->id`. The full replacement model is used: every field on `in` is
// taken as authoritative.
//
// Reconciliation:
//   * No fields besides `name` changed → AXEvent re-declare with the
//     new NiceName, no timer churn (FR-8.5 id-stable rename).
//   * Other fields changed → cancel armed timers for this id and
//     re-arm from the new spec. The topic itself stays declared; only
//     the firing path is re-evaluated.
int anchors_update(const anchor_t* in);

// Delete an operator-defined anchor by id. Built-ins return
// ANCHORS_ERR_BUILTIN. Anchors referenced by another anchor's
// event_source / start_event / end_event are NOT auto-deleted; the
// dangling reference is logged at WARN at recompute time and the
// referencing anchor is silently skipped on arm. (Active referential-
// integrity validation is left to the UI side per FR-11.5.)
int anchors_delete(const char* id);

// Toggle the FR-11.7 enable state for an anchor (built-in or
// operator-defined), persisting through `localdata/schedule_enabled.json`.
// This NEVER touches the AXEvent declaration table — the topic stays
// declared per DL-18. Returns ANCHORS_OK on success, ANCHORS_ERR_NOT_FOUND
// if no anchor (or calendar entry) with that id is currently registered,
// or ANCHORS_ERR_PERSIST on JSON write failure.
//
// Note: this entry point is shared with calendar entries — both go
// through `localdata/schedule_enabled.json` and the same FR-8.8 firing
// gate. The HTTP `events` endpoint dispatches here.
int anchors_set_enabled(const char* id, int enabled);

// Read the current FR-11.7 enable state for an anchor or calendar entry,
// consulting the in-memory cache populated from
// `localdata/schedule_enabled.json`. Absent keys → enabled (default-on).
// Returns 1 (enabled) or 0 (disabled). NULL or unknown id → 1, so the
// firing-path gate fails open if the cache hasn't been initialized yet.
int anchors_is_enabled(const char* id);

// Atomic bulk replacement of the operator-defined anchor list. Used by
// the POST /anchors "replace all" mode (and by future M7 import). The
// caller passes an array of `count` operator anchors; built-ins are
// untouched. Validation is all-or-nothing: any element failing
// individual validation rejects the entire batch with the first error
// encountered, leaving in-memory and on-disk state unchanged.
int anchors_replace_all(const anchor_t* operator_anchors, size_t count);

#ifdef __cplusplus
}
#endif

#endif  // CAMERA_SCHEDULE_ANCHORS_H
