// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Implementation of seasonal_next — equinox / solstice instants in UTC.
//
// References (all chapter/table numbers from Meeus 2nd ed.):
//   * Ch. 27 Table 27.B — JDE0 polynomial in y = (year - 2000) / 1000
//     for years +1000…+3000.
//   * Ch. 27 Table 27.C — 24 periodic-correction terms summed and added
//     to JDE0 to bring accuracy below ~60 s.
//   * Ch. 10 — ΔT (TT − UT). Espenak-Meeus 2005-2050 polynomial used
//     here covers our 1900-2100 target with adequate margin (errors
//     in the polynomial are well below our 60 s budget once you're
//     more than a few decades from 2000).
//
// The algorithm is closed-form: no iteration, no series convergence
// risk, no observer-dependent inputs. JDE0 polynomial gives a TT
// estimate accurate to ~70 s; the 24-term correction sharpens that to
// well under a minute. ΔT subtracts ~70-80 s for our era to convert
// TT → UT.

#include "seasonal.h"

#include <math.h>
#include <time.h>

// ---------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// JD epoch for 1970-01-01T00:00:00 UTC. Used to convert Julian Day to
// time_t: time_t = (JD - 2440587.5) * 86400.
static const double JD_UNIX_EPOCH = 2440587.5;

// J2000.0 epoch (TT). Meeus Table 27.C uses T = (JDE0 − 2451545.0)/36525
// to evaluate the periodic-correction series, where T is in Julian
// centuries from J2000.0.
static const double JD_J2000 = 2451545.0;

// Conversion factor: 1 Julian century = 36525 days.
static const double JULIAN_CENTURY_DAYS = 36525.0;

// ---------------------------------------------------------------------
// Meeus Table 27.B — JDE0 polynomial coefficients for years 1000..3000.
// Indexed by seasonal_kind_t (March eq, June sol, Sept eq, Dec sol).
//
// JDE0 = c0 + c1*y + c2*y^2 + c3*y^3 + c4*y^4
// where y = (year - 2000) / 1000.
// ---------------------------------------------------------------------

typedef struct {
    double c0, c1, c2, c3, c4;
} jde0_poly_t;

static const jde0_poly_t JDE0_TABLE_B[4] = {
    // March equinox
    { 2451623.80984, 365242.37404,  0.05169, -0.00411, -0.00057 },
    // June solstice
    { 2451716.56767, 365241.62603,  0.00325,  0.00888, -0.00030 },
    // September equinox
    { 2451810.21715, 365242.01767, -0.11575,  0.00337,  0.00078 },
    // December solstice
    { 2451900.05952, 365242.74049, -0.06223, -0.00823,  0.00032 }
};

// ---------------------------------------------------------------------
// Meeus Table 27.C — 24 periodic-correction terms (shared across all
// four event kinds). Each row contributes A * cos(B + C * T), where T
// is in Julian centuries from J2000.0 (TT).
//
// Sum S of all 24 terms is converted to JDE correction via:
//   W   = 35999.373 * T - 2.47          (degrees)
//   Δλ  = 1 + 0.0334 * cos(W) + 0.0007 * cos(2W)
//   JDE = JDE0 + (0.00001 * S) / Δλ     (correction in days)
// ---------------------------------------------------------------------

typedef struct {
    double A;
    double B;   // degrees
    double C;   // degrees per Julian century
} term_27c_t;

static const term_27c_t TERMS_27C[24] = {
    { 485.0, 324.96,   1934.136 },
    { 203.0, 337.23,  32964.467 },
    { 199.0, 342.08,     20.186 },
    { 182.0,  27.85, 445267.112 },
    { 156.0,  73.14,  45036.886 },
    { 136.0, 171.52,  22518.443 },
    {  77.0, 222.54,  65928.934 },
    {  74.0, 296.72,   3034.906 },
    {  70.0, 243.58,   9037.513 },
    {  58.0, 119.81,  33718.147 },
    {  52.0, 297.17,    150.678 },
    {  50.0,  21.02,   2281.226 },
    {  45.0, 247.54,  29929.562 },
    {  44.0, 325.15,  31555.956 },
    {  29.0,  60.93,   4443.417 },
    {  18.0, 155.12,  67555.328 },
    {  17.0, 288.79,   4562.452 },
    {  16.0, 198.04,  62894.029 },
    {  14.0, 199.76,  31436.921 },
    {  12.0,  95.39,  14577.848 },
    {  12.0, 287.11,  31931.756 },
    {  12.0, 320.81,  34777.259 },
    {   9.0, 227.73,   1222.114 },
    {   8.0,  15.45,  16859.074 }
};

#define TERM_COUNT (sizeof(TERMS_27C) / sizeof(TERMS_27C[0]))

// ---------------------------------------------------------------------
// ΔT (TT − UT) in seconds.
//
// Espenak & Meeus 2007 polynomial fits, NASA Eclipse pages. The
// piecewise polynomials used here cover 1900-2100 with errors well
// below our 60 s tolerance. Outside that range we still return a
// best-effort estimate so seasonal_next never fails for valid input.
// ---------------------------------------------------------------------

static double delta_t_seconds(int year) {
    // For years 1900-2100 we use four piecewise polynomials from
    // Espenak-Meeus, parameterized by `t = year - <pivot>`.
    if (year >= 1900 && year < 1920) {
        double t = year - 1900;
        return -2.79
             +  1.494119  * t
             - 0.0598939  * t * t
             + 0.0061966  * t * t * t
             - 0.000197   * t * t * t * t;
    }
    if (year >= 1920 && year < 1941) {
        double t = year - 1920;
        return  21.20
             +  0.84493   * t
             - 0.076100   * t * t
             + 0.0020936  * t * t * t;
    }
    if (year >= 1941 && year < 1961) {
        double t = year - 1950;
        return  29.07
             +  0.407     * t
             - t * t / 233.0
             + t * t * t / 2547.0;
    }
    if (year >= 1961 && year < 1986) {
        double t = year - 1975;
        return  45.45
             +  1.067     * t
             - t * t / 260.0
             - t * t * t / 718.0;
    }
    if (year >= 1986 && year < 2005) {
        double t = year - 2000;
        return  63.86
             +  0.3345    * t
             - 0.060374   * t * t
             + 0.0017275  * t * t * t
             + 0.000651814* t * t * t * t
             + 0.00002373599 * t * t * t * t * t;
    }
    if (year >= 2005 && year < 2050) {
        double t = year - 2000;
        return  62.92
             +  0.32217   * t
             + 0.005589   * t * t;
    }
    if (year >= 2050 && year <= 2150) {
        // Espenak-Meeus long-term parabolic. Note: 2050+ ΔT is a
        // genuine prediction with uncertainty on the order of tens of
        // seconds (IERS and Espenak-Meeus diverge by ~50 s at 2100).
        // FR-5's accuracy claim is calibrated for 1900-2050; values
        // returned beyond 2050 are best-effort only.
        double u = (year - 1820) / 100.0;
        double dt = -20.0 + 32.0 * u * u - 0.5628 * (2150 - year);
        return dt;
    }
    // Fall-through for years <1900 or >2150 — degraded but bounded.
    double u = (year - 1820) / 100.0;
    return -20.0 + 32.0 * u * u;
}

// ---------------------------------------------------------------------
// JDE0 + Table 27.C correction for a given event kind and Gregorian
// year. Returns the seasonal instant in Terrestrial Time (TT) as a
// Julian Day (JDE).
// ---------------------------------------------------------------------

static double seasonal_jde_tt(seasonal_kind_t kind, int year) {
    const jde0_poly_t* p = &JDE0_TABLE_B[kind];

    double y  = (double)(year - 2000) / 1000.0;
    double y2 = y * y;
    double y3 = y2 * y;
    double y4 = y3 * y;
    double jde0 = p->c0 + p->c1 * y + p->c2 * y2 + p->c3 * y3 + p->c4 * y4;

    // T in Julian centuries from J2000.0 (TT).
    double T = (jde0 - JD_J2000) / JULIAN_CENTURY_DAYS;

    // Sum the 24 periodic terms.
    double S = 0.0;
    for (size_t i = 0; i < TERM_COUNT; i++) {
        double angle_deg = TERMS_27C[i].B + TERMS_27C[i].C * T;
        double angle_rad = angle_deg * (M_PI / 180.0);
        S += TERMS_27C[i].A * cos(angle_rad);
    }

    double W_deg = 35999.373 * T - 2.47;
    double W_rad = W_deg * (M_PI / 180.0);
    double dlambda = 1.0 + 0.0334 * cos(W_rad) + 0.0007 * cos(2.0 * W_rad);

    double jde = jde0 + (0.00001 * S) / dlambda;
    return jde;
}

// ---------------------------------------------------------------------
// Convert a TT JDE for the given Gregorian year to a UTC time_t. Uses
// year-based ΔT (close enough — ΔT changes only ~1 s/year, dwarfed by
// the JDE0 polynomial residual).
// ---------------------------------------------------------------------

static time_t jde_tt_to_unix(double jde_tt, int year) {
    double dt = delta_t_seconds(year);
    double jd_ut = jde_tt - dt / 86400.0;
    double unix_seconds = (jd_ut - JD_UNIX_EPOCH) * 86400.0;

    // Round to nearest second. time_t is signed integral; floor() of a
    // negative double would round-toward-minus-infinity, but seasonal
    // events post-1970 are all positive, so a simple +0.5 trick works.
    if (unix_seconds >= 0)
        return (time_t)(unix_seconds + 0.5);
    return (time_t)(unix_seconds - 0.5);
}

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

int seasonal_next(time_t after, seasonal_kind_t kind, time_t* out) {
    if (!out) return -1;
    if (kind != SEASONAL_MARCH_EQUINOX
     && kind != SEASONAL_JUNE_SOLSTICE
     && kind != SEASONAL_SEPTEMBER_EQUINOX
     && kind != SEASONAL_DECEMBER_SOLSTICE) {
        return -1;
    }

    // Year of `after` in UTC. We try this year first, then fall through
    // to year+1 if the event has already passed. (Late-December queries
    // for a December solstice that's ~21 December and now's 25 December
    // exercise the +1 branch.)
    struct tm tm_utc;
    if (!gmtime_r(&after, &tm_utc)) return -1;
    int start_year = tm_utc.tm_year + 1900;

    for (int yr = start_year; yr <= start_year + 1; yr++) {
        double jde = seasonal_jde_tt(kind, yr);
        time_t when = jde_tt_to_unix(jde, yr);
        if (when > after) {
            *out = when;
            return 0;
        }
    }
    return -1;
}
