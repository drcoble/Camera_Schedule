// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Seasonal-event routine for the four annual instants used by FR-5:
// March equinox, June solstice, September equinox, December solstice.
//
// Like astro/solar.h and astro/lunar.h this module is intentionally
// pure: no time(NULL), no localtime(), no ACAP framework dependency.
// The host-side test harness links it directly. Callers (timers.c
// on-camera, test_seasonal.c on host) supply all instants.
//
// API shape note. Seasonal events are observer-independent point-in-
// time instants — like lunar phases, the *moment* of equinox or
// solstice does not depend on the observer's location. Only the
// user-facing *label* of June/December solstice depends on hemisphere
// (FR-5.2), and that mapping is applied at event-registration time in
// main.c, not in this module.
//
// The events occur once per Gregorian year (~91-92 days apart). The
// scheduler in timers.c uses the same arm-and-re-arm-on-fire pattern
// that the lunar-phase slots use, so this module exposes a single
// "next-after-T" entry point matching `lunar_next_phase`.

#ifndef CAMERA_SCHEDULE_SEASONAL_H
#define CAMERA_SCHEDULE_SEASONAL_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// The four seasonal events the user can bind action rules to. Ordering
// matches their ecliptic-longitude progression (0°, 90°, 180°, 270°)
// and chronological order within a typical Gregorian year.
typedef enum {
    SEASONAL_MARCH_EQUINOX     = 0,  // Sun at ecliptic longitude 0°
    SEASONAL_JUNE_SOLSTICE     = 1,  // Sun at ecliptic longitude 90°
    SEASONAL_SEPTEMBER_EQUINOX = 2,  // Sun at ecliptic longitude 180°
    SEASONAL_DECEMBER_SOLSTICE = 3   // Sun at ecliptic longitude 270°
} seasonal_kind_t;

// Find the UTC instant of the next event of the given kind that occurs
// strictly after `after`. The pattern mirrors lunar_next_phase: arm a
// timer for the result, then on fire re-query with `after = now`.
//
//   after  any UTC instant; the next event of `kind` that is strictly
//          greater than this is returned.
//   kind   one of the four seasonal_kind_t values.
//   out    pre-allocated; written on success, untouched on failure.
//
// Return value:
//   0   success; *out written
//  -1   invalid input (NULL out, unrecognized kind) or numerical
//       failure.
//
// Accuracy. <= 60 s for years 1900-2050, using Meeus *Astronomical
// Algorithms* 2nd ed. ch. 27 Table 27.B polynomial + Table 27.C
// 24-term periodic correction series, with Espenak-Meeus ΔT applied
// to convert TT to UTC. Matches the FR-3.7-equivalent budget for
// solar/seasonal events. Years beyond 2050 are best-effort: ΔT
// prediction uncertainty (tens of seconds) dominates the polynomial
// residual past that horizon.
int seasonal_next(time_t after, seasonal_kind_t kind, time_t* out);

#ifdef __cplusplus
}
#endif

#endif  // CAMERA_SCHEDULE_SEASONAL_H
