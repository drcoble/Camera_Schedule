// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// See status.h for the contract.

#include "status.h"

#include <glib.h>
#include <string.h>

// Ring buffer with a write-cursor. We never compact; the cursor wraps
// modulo CAPACITY and `g_count` saturates at CAPACITY. A snapshot read
// produces a flat newest-first array by walking back from `cursor - 1`.

static GMutex              g_lock;
static int                 g_initialized = 0;
static recompute_summary_t g_ring[STATUS_RING_CAPACITY];
static int                 g_cursor      = 0;     // next write slot
static int                 g_count       = 0;     // valid entries (≤ CAPACITY)
static recompute_summary_t g_snapshot[STATUS_RING_CAPACITY];

static time_t              g_next_utc    = 0;
static recompute_trigger_t g_next_reason = RECOMPUTE_TRIGGER_MIDNIGHT;

void status_init(void) {
    if (g_initialized) return;
    g_mutex_init(&g_lock);
    memset(g_ring, 0, sizeof g_ring);
    g_cursor = 0;
    g_count  = 0;
    g_next_utc    = 0;
    g_next_reason = RECOMPUTE_TRIGGER_MIDNIGHT;
    g_initialized = 1;
}

void status_record(const recompute_summary_t* s) {
    if (!s || !g_initialized) return;
    g_mutex_lock(&g_lock);
    g_ring[g_cursor] = *s;
    g_cursor = (g_cursor + 1) % STATUS_RING_CAPACITY;
    if (g_count < STATUS_RING_CAPACITY) g_count++;
    g_mutex_unlock(&g_lock);
}

const recompute_summary_t* status_last(void) {
    if (!g_initialized) return NULL;
    g_mutex_lock(&g_lock);
    if (g_count == 0) { g_mutex_unlock(&g_lock); return NULL; }
    int last_idx = (g_cursor - 1 + STATUS_RING_CAPACITY) % STATUS_RING_CAPACITY;
    // Stage into the snapshot[0] slot so the returned pointer is stable
    // outside the lock. Using the snapshot buffer here would race with
    // a concurrent status_recent caller — but status_last and
    // status_recent are not called in a tight overlap from the same
    // handler (HTTP /status reads last via status_last AND recent via
    // status_recent in sequence on one thread). So copy into a dedicated
    // single-entry slot.
    static recompute_summary_t s_last_buf;
    s_last_buf = g_ring[last_idx];
    g_mutex_unlock(&g_lock);
    return &s_last_buf;
}

const recompute_summary_t* status_recent(int* count_out) {
    if (!g_initialized || !count_out) {
        if (count_out) *count_out = 0;
        return NULL;
    }
    g_mutex_lock(&g_lock);
    int n = g_count;
    // Newest-first walk: start at (cursor-1) and go backwards `n` steps.
    int idx = (g_cursor - 1 + STATUS_RING_CAPACITY) % STATUS_RING_CAPACITY;
    for (int i = 0; i < n; i++) {
        g_snapshot[i] = g_ring[idx];
        idx = (idx - 1 + STATUS_RING_CAPACITY) % STATUS_RING_CAPACITY;
    }
    g_mutex_unlock(&g_lock);
    *count_out = n;
    return g_snapshot;
}

const char* status_trigger_str(recompute_trigger_t t) {
    switch (t) {
        case RECOMPUTE_TRIGGER_BOOT:             return "boot";
        case RECOMPUTE_TRIGGER_MIDNIGHT:         return "midnight";
        case RECOMPUTE_TRIGGER_LOCATION_CHANGE:  return "location_change";
        case RECOMPUTE_TRIGGER_MANUAL:           return "manual";
        case RECOMPUTE_TRIGGER_CONFIG_CHANGE:    return "config_change";
        case RECOMPUTE_TRIGGER_IMPORT:           return "import";
        case RECOMPUTE_TRIGGER_TZ_CHANGE:        return "tz_change";
    }
    return "unknown";
}

void status_set_next(time_t scheduled_utc, recompute_trigger_t reason) {
    if (!g_initialized) return;
    g_mutex_lock(&g_lock);
    g_next_utc    = scheduled_utc;
    g_next_reason = reason;
    g_mutex_unlock(&g_lock);
}

void status_get_next(time_t* out_utc, recompute_trigger_t* out_reason) {
    if (!out_utc || !out_reason) return;
    if (!g_initialized) {
        *out_utc = 0;
        *out_reason = RECOMPUTE_TRIGGER_MIDNIGHT;
        return;
    }
    g_mutex_lock(&g_lock);
    *out_utc    = g_next_utc;
    *out_reason = g_next_reason;
    g_mutex_unlock(&g_lock);
}
