// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Daily-recompute + per-event one-shot timer scheduler.
//
// Public surface deliberately small: `timers_init` is called once at
// boot after ACAP() is initialized; everything else (midnight wake-up,
// per-event firing, geolocation-change recompute) is internal. This
// follows Timelapse2's pattern (DL-06) but adds zenith-parameterized
// solar inputs from astro/solar.h.

#ifndef CAMERA_SCHEDULE_TIMERS_H
#define CAMERA_SCHEDULE_TIMERS_H

#ifdef __cplusplus
extern "C" {
#endif

// Initialize and arm the daily recompute machinery.
//
// On call:
//   1. Reads camera lat/lon via ACAP_DEVICE_Latitude/Longitude.
//   2. Computes today's solar events at the standard sunrise/sunset
//      zenith via solar_compute().
//   3. For each event whose computed UTC time is still in the future,
//      arms a one-shot GLib timer that fires the corresponding ACAP
//      event topic via ACAP_EVENTS_Fire().
//   4. Arms a midnight GLib timer that re-runs steps 1-3 when local
//      civil midnight rolls over.
//
// Must be called after ACAP() and after the events have been declared
// by the framework (settings/events.json -> ACAP_EVENTS_Add_Event).
//
// Returns 0 on success, -1 on initialization failure (in which case
// the camera will stay alive but no events will fire — main.c logs
// the failure and the camera stays available for diagnostic HTTP).
int timers_init(void);

// Drop all timer sources. Safe to call from the SIGTERM handler before
// quitting the main loop. Idempotent.
void timers_cleanup(void);

// Force an immediate recompute + re-arm. Used by location changes (M2)
// and the manual recompute endpoint (M7). Returns 0 on success.
int timers_recompute_now(void);

#ifdef __cplusplus
}
#endif

#endif  // CAMERA_SCHEDULE_TIMERS_H
