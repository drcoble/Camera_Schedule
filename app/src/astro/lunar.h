// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Lunar-position routines for moonrise/moonset/transit/anti-transit,
// principal phases (new / first quarter / full / last quarter), and
// illumination fraction. Implementation strategy: Meeus
// "Astronomical Algorithms", 2nd ed., chapters 47 (lunar position),
// 48 (illuminated fraction), and 49 (phases).
//
// Like astro/solar.h, this module is intentionally pure: no time(NULL),
// no localtime(), no ACAP framework dependency. The host-side test
// harness links it directly. The caller (timers.c on-camera,
// test_lunar.c on host) supplies all dates and instants.
//
// API shape note. Daily events (rise/set/transit/anti-transit) follow
// the same UTC-day-window convention as solar.h. Phase events are
// instants and use a separate "next-after-T" API because they are not
// daily — the four phases of the lunar cycle each occur ~once per
// synodic month (~29.53 d). The phase scheduler in timers.c re-arms
// after each fire by querying for the next instant of the same kind.

#ifndef CAMERA_SCHEDULE_LUNAR_H
#define CAMERA_SCHEDULE_LUNAR_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returned for daily-event fields (moonrise / moonset) on days where
// the Moon does not cross the horizon at all — common in polar
// regions, and possible at any latitude on roughly 1 day in 30 because
// the Moon transits the meridian about 50 minutes later each day, so
// either a rise or a set can drop out of the 24h UTC window.
//
// Negative time_t values are reserved by POSIX as error indicators, so
// LUNAR_NO_EVENT is unambiguous against any valid result.
#define LUNAR_NO_EVENT ((time_t)-1)

// The four principal phases that the user can bind action rules to.
// Ordering matches Meeus ch. 49 enumeration (k modulo 4):
//   0  new moon          (Sun-Earth-Moon angle 0°,   illumination ~0)
//   1  first quarter     (90°,                       illumination ~0.5 waxing)
//   2  full moon         (180°,                      illumination ~1)
//   3  last quarter      (270°,                      illumination ~0.5 waning)
typedef enum {
    LUNAR_PHASE_NEW           = 0,
    LUNAR_PHASE_FIRST_QUARTER = 1,
    LUNAR_PHASE_FULL          = 2,
    LUNAR_PHASE_LAST_QUARTER  = 3
} lunar_phase_t;

// Daily lunar events for a given UTC date.
typedef struct {
    time_t moonrise;             // UTC. LUNAR_NO_EVENT if the Moon
                                 //      does not rise during the UTC
                                 //      day at this lat/lon.
    time_t moonset;              // UTC. LUNAR_NO_EVENT if no set today.
    time_t lunar_transit;        // UTC. Always defined: the Moon
                                 //      crosses the local meridian
                                 //      ~once every 24 h 50 m. The
                                 //      transit reported is the one
                                 //      whose UTC instant falls inside
                                 //      the day window (or, if none
                                 //      does on a "skipped" day, the
                                 //      nearest one — see notes).
    time_t lunar_anti_transit;   // UTC. Lower culmination — the Moon
                                 //      crossing the antimeridian.
                                 //      Always defined under the same
                                 //      windowing rule as transit.
                                 //      NOT exactly transit ± 12 h
                                 //      (lunar day ~24 h 50 m, so the
                                 //      gap is ~12 h 25 m and varies).
} lunar_events_t;

// Compute daily lunar events for (lat, lon) on the given Gregorian
// UTC date.
//
// The date selects the 24-hour UTC window [00:00, 24:00) of `year-
// month-day`. moonrise / moonset are reported only if their UTC instant
// falls within that window; otherwise LUNAR_NO_EVENT. transit / anti-
// transit are computed similarly; on the rare day where neither
// transit/anti-transit lies in the window (because both have shifted
// out by the ~50 min/day drift), the function returns the nearest
// instant within ±13 h of UTC noon of the input date so callers always
// get a usable value to arm a timer against.
//
//   lat   degrees, north positive, in [-90, 90]
//   lon   degrees, east positive,  in [-180, 180]
//   year  Gregorian year (AD), >= 1970
//   month [1..12]
//   day   [1..31] (validated against month + leap-year rules)
//   out   pre-allocated by caller; all fields written
//
// Return value:
//   0   success
//  -1   invalid input. Every `out` field is set to LUNAR_NO_EVENT.
//
// Accuracy.
//   * Moonrise / moonset: <= 2 min for habitable latitudes (FR-4.5),
//     using the Meeus ch. 47 truncated periodic series and topocentric
//     parallax with hourly altitude sampling and linear interpolation
//     near the horizon crossing.
//   * Transit / anti-transit: <= 2 min.
//   * Polar latitudes (|lat| > 80°) degrade gracefully: rise/set
//     return LUNAR_NO_EVENT for stretches of polar lunar day/night,
//     while transit/anti-transit remain defined.
int lunar_compute_daily(double lat, double lon,
                        int year, int month, int day,
                        lunar_events_t* out);

// Find the UTC instant of the next phase of the given kind that occurs
// strictly after `after`. Useful for one-shot phase timers in
// timers.c: arm a timer for the result, then on fire re-query with
// `after = now`.
//
// Return value:
//   0   success; *out written
//  -1   invalid input (NULL out, unrecognized phase) or numerical
//       failure. *out untouched.
//
// Accuracy. <= 5 min per FR-4.5 over 1900-2100, using Meeus ch. 49
// with the truncated periodic correction series.
int lunar_next_phase(time_t after, lunar_phase_t kind, time_t* out);

// Compute the Moon's illuminated fraction at the given instant, in
// [0.0, 1.0]. Pure geometric quantity (Sun-Earth-Moon phase angle),
// independent of observer location.
//
// Accuracy: <= 0.02 absolute (FR-4.5 says 2 percentage points).
//
// On numerical failure (extremely unlikely for any year >= 1970 the
// platform's time_t can represent), returns -1.0 — the caller can
// distinguish from any valid result by `result < 0`.
double lunar_illumination(time_t when);

#ifdef __cplusplus
}
#endif

#endif  // CAMERA_SCHEDULE_LUNAR_H
