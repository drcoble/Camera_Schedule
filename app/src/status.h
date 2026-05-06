// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Status ring buffer — last 50 recompute summaries (FR-13.3) plus the
// next-recompute schedule. Read by the GET /status handler in main.c
// and written by the recompute pipeline in timers.c.
//
// Threading: the ring buffer is touched both from the GLib main loop
// (recompute_today via the midnight GSource) and from the FastCGI
// worker thread (manual recompute through POST /recompute and the
// status read on GET /status). All accessors take an internal GMutex.

#ifndef CAMERA_SCHEDULE_STATUS_H
#define CAMERA_SCHEDULE_STATUS_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Capacity matches FR-13.3 ("last 50 recompute summaries"). Sized as a
// macro so test fixtures can `#define STATUS_RING_CAPACITY` lower
// before including this header.
#ifndef STATUS_RING_CAPACITY
#define STATUS_RING_CAPACITY 50
#endif

typedef enum {
    RECOMPUTE_TRIGGER_BOOT             = 0,
    RECOMPUTE_TRIGGER_MIDNIGHT         = 1,
    RECOMPUTE_TRIGGER_LOCATION_CHANGE  = 2,
    RECOMPUTE_TRIGGER_MANUAL           = 3,
    RECOMPUTE_TRIGGER_CONFIG_CHANGE    = 4,
    RECOMPUTE_TRIGGER_IMPORT           = 5,
    RECOMPUTE_TRIGGER_TZ_CHANGE        = 6
} recompute_trigger_t;

typedef struct {
    time_t              started_at_utc;
    recompute_trigger_t trigger;
    int                 elapsed_ms;
    int                 anchors_evaluated;
    int                 events_armed;
    int                 skipped_polar;
    int                 skipped_disabled;
    int                 skipped_past;
    int                 errors;
} recompute_summary_t;

// Initialize the ring (zero entries, mutex constructed). Idempotent.
// MUST be called before status_record / status_last / status_recent.
void status_init(void);

// Append a recompute summary (newest-first ordering on read). Copies
// the supplied struct; caller may free / reuse `s` immediately.
void status_record(const recompute_summary_t* s);

// Pointer to the most-recent entry, or NULL if no recompute has run.
// Pointer is valid until the next status_record call. Callers in the
// HTTP path are expected to copy fields out before responding.
const recompute_summary_t* status_last(void);

// Snapshot the ring into an internal newest-first array and return a
// pointer + count. `*count_out` ≤ STATUS_RING_CAPACITY. Pointer remains
// valid until the next status_recent call from this thread (the
// snapshot buffer is module-scope, returned-by-pointer; callers MUST
// finish reading it before anyone else calls back in). Concurrent
// readers are serialized by the same mutex protecting the ring.
const recompute_summary_t* status_recent(int* count_out);

// Map a trigger enum to the canonical lowercase JSON string
// ("boot","midnight","location_change","manual","config_change",
// "import","tz_change"). Returns "unknown" on out-of-range input.
const char* status_trigger_str(recompute_trigger_t t);

// Record the next scheduled recompute (used by the midnight timer arm
// and the lunar phase / season / anchor schedulers). Calls in succession
// overwrite — the latest call wins. Pass `(time_t)0` to clear.
void status_set_next(time_t scheduled_utc, recompute_trigger_t reason);

// Read the next scheduled recompute. `*out_utc` and `*out_reason` MUST
// be non-NULL; on first call before any status_set_next, `*out_utc`
// is set to 0 and `*out_reason` to RECOMPUTE_TRIGGER_MIDNIGHT.
void status_get_next(time_t* out_utc, recompute_trigger_t* out_reason);

#ifdef __cplusplus
}
#endif

#endif  // CAMERA_SCHEDULE_STATUS_H
