// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Solar-position routine for sunrise/sunset and zenith-N twilights.
// Refactored from Timelapse2's sunevents.c (Fred Juhlin, MIT, 2024)
// to be parameterized by zenith angle and to return a SOLAR_NO_EVENT
// sentinel at polar latitudes where acos() would overflow.
//
// This module is intentionally pure: it does not call time(NULL) or
// localtime() and does not depend on the camera environment, so it
// can be linked into the host-side test harness without the ACAP
// framework. The caller (timers.c on-camera, test_solar.c on host)
// supplies the date.

#ifndef CAMERA_SCHEDULE_SOLAR_H
#define CAMERA_SCHEDULE_SOLAR_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returned in the sunrise/sunset fields when no event of the requested
// zenith occurs on the given date — polar day/night, or zenith-induced
// continuous twilight at high latitudes for the wider zeniths.
//
// Negative time_t values are reserved by POSIX as error indicators, so
// SOLAR_NO_EVENT is unambiguous against any valid result.
#define SOLAR_NO_EVENT ((time_t)-1)

// Standard zenith angles (degrees from local zenith). The
// 90.833° value is the standard sunrise/sunset definition: 90° geometric
// + 0.566° for atmospheric refraction at the horizon + 0.267° for the
// solar disk's apparent radius. The three twilight altitudes match
// the conventional definitions (FR-3 in the requirements).
#define SOLAR_ZENITH_SUNRISE_SUNSET         90.833
#define SOLAR_ZENITH_CIVIL_TWILIGHT          96.0
#define SOLAR_ZENITH_NAUTICAL_TWILIGHT      102.0
#define SOLAR_ZENITH_ASTRONOMICAL_TWILIGHT  108.0

typedef struct {
    time_t sunrise;     // UTC. SOLAR_NO_EVENT iff sun stays outside the
                        //      zenith band all day at this lat/date.
    time_t sunset;      // UTC. Same NO_EVENT condition as sunrise.
    time_t solar_noon;  // UTC. Always defined — sun crosses the local
                        //      meridian once per solar day regardless
                        //      of latitude.
} solar_events_t;

// Compute the solar events for (lat, lon) on the given Gregorian date
// at the given solar zenith.
//
// The date is interpreted as UTC: the function returns the sunrise,
// solar_noon, and sunset that occur on or near 00:00 UTC of the input
// date. For habitable latitudes and longitudes |lon| < 180° the events
// fall within ±15 hours of UTC midnight, well inside the same UTC day
// as solar_noon.
//
//   lat          degrees, north positive, in [-90, 90]
//   lon          degrees, east positive,  in [-180, 180]
//   year         Gregorian year (AD), >= 1970
//   month        [1..12]
//   day          [1..31] (validated against month + leap-year rules)
//   zenith_deg   typically one of SOLAR_ZENITH_* above
//   out          pre-allocated by caller; all fields written
//
// Return value:
//   0           success
//  -1           invalid input — out-of-range lat/lon, malformed date,
//               or NULL out. Every `out` field is set to SOLAR_NO_EVENT.
//
// Accuracy. Uses the NOAA/Spencer fractional-year expansion for solar
// declination and the equation of time, evaluated at 12 UTC of the
// input date. Empirical bound: |error| <= 60 s for |lat| <= 60° at the
// standard sunrise/sunset zenith, which satisfies FR-3.7. Accuracy
// degrades gracefully at higher latitudes; near polar latitudes the
// SOLAR_NO_EVENT path is taken before the error-amplification regime.
int solar_compute(double lat, double lon,
                  int year, int month, int day,
                  double zenith_deg,
                  solar_events_t* out);

#ifdef __cplusplus
}
#endif

#endif  // CAMERA_SCHEDULE_SOLAR_H
