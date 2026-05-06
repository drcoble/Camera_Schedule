// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Implementation: see solar.h for the public-API contract.
//
// Algorithm. Meeus, "Astronomical Algorithms," chapters 22 (sidereal
// time), 25 (solar coordinates), and the equation-of-time form
// derived in chapter 28. Given a Gregorian date (UTC), we:
//
//   1. Compute the Julian Day at 12 UTC of the input date.
//   2. Derive Sun's apparent geocentric longitude and declination
//      via the standard mean-longitude / mean-anomaly / equation-of-
//      center expansion. Nutation is approximated by the omega
//      term — adequate for the FR-3.7 ±60 s budget.
//   3. Compute the equation of time via the closed-form expansion
//      from Meeus 28.3.
//   4. Plug declination into the standard hour-angle formula at the
//      requested zenith and produce sunrise / solar-noon / sunset as
//      UTC time_t values.
//
// Differences from Timelapse2's sunevents.c (and from the simpler
// Spencer-harmonic NOAA approximation we tried first):
//   * Spencer's expansion drifts up to ~3 minutes in early March at
//     mid-latitudes; Meeus stays under 60 s for |lat| ≤ 60° at the
//     standard sunrise/sunset zenith, validated against USNO.
//   * Zenith is parameterized rather than hard-coded.
//   * The function is pure (no time/localtime calls), reentrant, and
//     trivially testable on the host.
//   * Polar / extreme-zenith conditions return SOLAR_NO_EVENT instead
//     of letting acos() return NaN.

#define _GNU_SOURCE
#include "solar.h"

#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG_TO_RAD (M_PI / 180.0)
#define RAD_TO_DEG (180.0 / M_PI)

// ---- Helpers ------------------------------------------------------

// Howard Hinnant's "days from civil" — converts a Gregorian date to
// the count of days since 1970-01-01 (negative for earlier). Public
// domain; exact for the entire valid range of time_t.
// http://howardhinnant.github.io/date_algorithms.html#days_from_civil
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5
                       + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static int valid_date(int y, int m, int d) {
    if (y < 1970) return 0;
    if (m < 1 || m > 12) return 0;
    if (d < 1) return 0;
    static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max_day = mdays[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)))
        max_day = 29;
    return d <= max_day;
}

// Reduce an angle in degrees to [0, 360).
static double mod_deg(double x) {
    double r = fmod(x, 360.0);
    return r < 0.0 ? r + 360.0 : r;
}

// Julian Day at 12 UTC of the given Gregorian date.
// JD 2451545 = 2000-01-01 12 UTC (J2000.0).
static double jd_at_noon_utc(int y, int m, int d) {
    int64_t days_from_unix = days_from_civil(y, (unsigned)m, (unsigned)d);
    // Unix epoch (1970-01-01 00 UTC) sits at JD 2440587.5.
    // We want JD at 12 UTC of the same civil date, so add 0.5 + 0 days
    // for the noon offset on top of the 00 UTC anchor:
    //   JD(00 UTC) = 2440587.5 + days_from_unix
    //   JD(12 UTC) = JD(00 UTC) + 0.5
    return 2440587.5 + (double)days_from_unix + 0.5;
}

// ---- Meeus solar coordinates --------------------------------------

// Compute the Sun's apparent declination (radians) and the equation
// of time (minutes) at 12 UTC of the input date. Both are slow-varying
// over a day, so evaluating at noon is good enough for sunrise/sunset
// at any longitude — residual drift stays well under our 60 s budget.
static void solar_position_at_noon(int year, int month, int day,
                                   double* decl_rad,
                                   double* eot_minutes) {
    double JD = jd_at_noon_utc(year, month, day);
    double T  = (JD - 2451545.0) / 36525.0;            // Julian centuries since J2000

    // Mean longitude of the Sun (Meeus 25.2)
    double L0 = 280.46646
              + (36000.76983 + 0.0003032 * T) * T;
    L0 = mod_deg(L0);

    // Mean anomaly of the Sun (Meeus 25.3)
    double M = 357.52911
             + (35999.05029 - 0.0001537 * T) * T;
    double M_rad = mod_deg(M) * DEG_TO_RAD;

    // Orbital eccentricity (Meeus 25.4)
    double e = 0.016708634
             - (0.000042037 + 0.0000001267 * T) * T;

    // Sun's equation of center (Meeus p. 164)
    double C = (1.914602 - (0.004817 + 0.000014 * T) * T) * sin(M_rad)
             + (0.019993 - 0.000101 * T)               * sin(2.0 * M_rad)
             +  0.000289                                * sin(3.0 * M_rad);

    // True longitude (deg)
    double trueL = L0 + C;

    // Longitude of the ascending node of the Moon's mean orbit on the
    // ecliptic — used to approximate nutation in longitude/obliquity.
    // (Meeus p. 144)
    double omega_rad = (125.04 - 1934.136 * T) * DEG_TO_RAD;

    // Apparent longitude of the Sun (deg)
    double apparentL = trueL - 0.00569 - 0.00478 * sin(omega_rad);
    double apparentL_rad = apparentL * DEG_TO_RAD;

    // Mean obliquity of the ecliptic in arcseconds, then deg.
    // (Meeus 22.2 in arcseconds; we evaluate at the input date's T.)
    double eps0_arcsec = 84381.448
                       + (-46.8150
                          + (-0.00059
                             +  0.001813 * T) * T) * T;
    double eps0 = eps0_arcsec / 3600.0;

    // Corrected obliquity (deg) — first-order nutation in obliquity.
    double eps = eps0 + 0.00256 * cos(omega_rad);
    double eps_rad = eps * DEG_TO_RAD;

    *decl_rad = asin(sin(eps_rad) * sin(apparentL_rad));

    // Equation of time (Meeus 28.3) — radians, then convert via the
    // standard "1 rad time = 4 minutes" factor (since RAD_TO_DEG/15
    // converts radian-of-rotation to hours, and *60 to minutes; same
    // as multiplying by 4 * RAD_TO_DEG).
    double L0_rad = L0 * DEG_TO_RAD;
    double y      = tan(eps_rad / 2.0);
    y *= y;

    double E = y    * sin(2.0 * L0_rad)
             - 2.0  * e * sin(M_rad)
             + 4.0  * e * y * sin(M_rad) * cos(2.0 * L0_rad)
             - 0.5  * y * y * sin(4.0 * L0_rad)
             - 1.25 * e * e * sin(2.0 * M_rad);

    *eot_minutes = E * 4.0 * RAD_TO_DEG;
}

// ---- Public API ---------------------------------------------------

int solar_compute(double lat, double lon,
                  int year, int month, int day,
                  double zenith_deg,
                  solar_events_t* out) {
    if (!out) return -1;
    out->sunrise        = SOLAR_NO_EVENT;
    out->sunset         = SOLAR_NO_EVENT;
    out->solar_noon     = SOLAR_NO_EVENT;
    out->solar_midnight = SOLAR_NO_EVENT;

    if (lat < -90.0 || lat > 90.0)   return -1;
    if (lon < -180.0 || lon > 180.0) return -1;
    if (!valid_date(year, month, day)) return -1;
    if (!isfinite(zenith_deg))         return -1;

    double decl_rad, eot_min;
    solar_position_at_noon(year, month, day, &decl_rad, &eot_min);

    // Solar noon in UTC, expressed as fractional hours since UTC
    // midnight of the input date. The sun crosses the meridian of
    // `lon` at the moment apparent solar time = 12:00.
    double noon_hours = 12.0 - (lon / 15.0) - (eot_min / 60.0);

    int64_t midnight_secs = days_from_civil(year, (unsigned)month, (unsigned)day)
                          * (int64_t)86400;

    out->solar_noon = (time_t)(midnight_secs
                               + (int64_t)llround(noon_hours * 3600.0));

    // Solar midnight is the anti-transit — sun crossing the lower
    // meridian, 12 hours before/after solar noon. Convention here:
    // pick the one in the early hours of the input civil date
    // (i.e. noon − 12h). FR-3.3 — always defined, including polar.
    out->solar_midnight = out->solar_noon - 43200;

    double zenith_rad = zenith_deg * DEG_TO_RAD;
    double lat_rad    = lat * DEG_TO_RAD;
    double cos_lat    = cos(lat_rad);
    double cos_decl   = cos(decl_rad);

    if (cos_lat == 0.0 || cos_decl == 0.0) {
        // Geographic pole. Solar noon is still defined; sunrise/
        // sunset are not.
        return 0;
    }

    double cos_h = (cos(zenith_rad) - sin(lat_rad) * sin(decl_rad))
                 / (cos_lat * cos_decl);

    if (cos_h > 1.0 || cos_h < -1.0) {
        // Polar night (cos_h > 1: sun never rises above zenith) or
        // polar day (cos_h < -1: sun never sets below zenith). The
        // caller — timers.c — must skip arming today's timer for
        // sunrise/sunset and emit an INFO log per FR-3 / DL-15.
        return 0;
    }

    double h_hours = acos(cos_h) * RAD_TO_DEG / 15.0;

    out->sunrise = (time_t)(midnight_secs
                            + (int64_t)llround((noon_hours - h_hours) * 3600.0));
    out->sunset  = (time_t)(midnight_secs
                            + (int64_t)llround((noon_hours + h_hours) * 3600.0));
    return 0;
}
