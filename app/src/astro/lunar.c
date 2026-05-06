// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Implementation: see lunar.h for the public-API contract.
//
// Algorithm. Meeus, "Astronomical Algorithms", 2nd ed.:
//   * ch. 11  geocentric vs. geodetic latitude (parallax helpers)
//   * ch. 12  Greenwich apparent sidereal time
//   * ch. 13  parallax-in-altitude
//   * ch. 22  obliquity, nutation
//   * ch. 25  solar coordinates (used by phase-angle and ch. 49)
//   * ch. 47  lunar position (truncated periodic series)
//   * ch. 48  illuminated fraction
//   * ch. 49  phase JDE formula, periodic correction series
//
// The module is intentionally pure (no time(NULL)/localtime/syslog/
// printf, no ACAP deps). The host-side test harness links it directly
// against libm; on-camera the same object is linked into timers.c.
//
// Why the closed-form acos() trick used by solar.c does NOT work here.
// The Moon's declination changes by up to ~0.5°/h (vs. ~1°/day for the
// Sun) and its horizontal parallax (~0.95°) is altitude-dependent
// because parallax shifts apparent altitude by HP·cos(h). So we need
// numerical sampling: compute topocentric altitude h(t) at hourly
// steps, find sign changes of (h - h0), and linearly interpolate.
// Per FR-4.5 the 2-min accuracy target is comfortably met with a
// single Newton-style refinement after the linear bracket — verified
// against USNO references in test_lunar.c.

#define _GNU_SOURCE
#include "lunar.h"

#include <math.h>
#include <stdint.h>
#include <stddef.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG_TO_RAD (M_PI / 180.0)
#define RAD_TO_DEG (180.0 / M_PI)

// WGS-84 flattening (a = 6378137 m, 1/f = 298.257223563). Used to
// convert geodetic latitude to geocentric for topocentric parallax
// (Meeus ch. 11). The Moon's parallax is large enough (~3500 km
// position shift between observers) that ignoring flattening would
// add ~10 km / ~4 arcsec along the rise/set direction — small but
// worth folding in given how easy it is.
#define WGS84_FLATTENING (1.0 / 298.257223563)

// Standard refraction at the horizon (Meeus eq. 15.1, p. 106): the
// apparent altitude of an object whose true altitude is 0° is +34'.
//
// Because lunar_topocentric_alt() already applies the full
// geocentric->topocentric parallax shift, the rise/set threshold we
// compare against is the *topocentric* form:
//
//     h0 = -0.2725·HP - 34'
//
// At the moment of observed rise/set, the topocentric altitude of the
// Moon's center sits below the geometric horizon by (apparent
// semi-diameter) + (atmospheric refraction). 0.2725·HP is the
// apparent semi-diameter (USNO/Meeus convention — the Moon's physical
// radius is ~0.2725 times its mean distance, and HP is the angular
// radius of the Earth from the Moon, so 0.2725·HP closely approximates
// the apparent semi-diameter at the observer's instant); 34' is
// standard refraction. Both terms are subtractive — the image is
// lifted by refraction and the disk extends above center by the
// semi-diameter, so the center has to be that far below to put the
// upper limb at the geometric horizon.
//
// NOTE: do NOT use Meeus p. 102's geocentric form (h0 = 0.7275·HP -
// 34'). That form is for routines that compare against the *geocentric*
// altitude and bake the parallax shift into the threshold. Combining
// it with topocentric altitude double-counts the ~0.95° lunar
// parallax, pushing rise late and set early by minutes (more at high
// latitude where dh/dt is small).
#define STANDARD_REFRACTION_DEG (34.0 / 60.0)

// ---- Date / time helpers ------------------------------------------

// Howard Hinnant's "days from civil" — converts a Gregorian date to
// the count of days since 1970-01-01. Public domain; same routine
// used by solar.c.
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

// Convert a Unix epoch seconds value to JD. JD 2440587.5 = 1970-01-01
// 00 UTC.
static double jd_from_unix(double t_unix) {
    return 2440587.5 + t_unix / 86400.0;
}

// Convert a JD to Unix epoch seconds.
static double unix_from_jd(double jd) {
    return (jd - 2440587.5) * 86400.0;
}

// ---- Lunar position (Meeus ch. 47) --------------------------------
//
// We use the full truncated periodic series from Meeus tables 47.A
// (longitude / distance) and 47.B (latitude). The longitude/distance
// table contains 60 terms; latitude has 60 terms. This is deliberately
// large because the leading terms at the few-arcminute level are
// numerous. Truncating below this level pushes rise/set error past the
// 2-min FR-4.5 budget at high latitudes where dh/dt is small near the
// horizon.

typedef struct {
    int8_t  d, m, mp, f;   // multipliers for D, M, M', F
    int32_t coeff_l;       // coefficient for longitude/distance term
                           // (Meeus stores ΣL in 10^-6 deg, ΣR in
                           // 10^-3 km; we keep them as the raw integer
                           // and scale at the call site)
    int32_t coeff_r;
} lr_term_t;

typedef struct {
    int8_t  d, m, mp, f;
    int32_t coeff_b;       // coefficient for latitude term, 10^-6 deg
} b_term_t;

// Meeus table 47.A (Σl in 10^-6 °, Σr in 10^-3 km). Terms with M
// nonzero are scaled by E (or E^2) at evaluation time.
static const lr_term_t LR_TERMS[] = {
    {0,  0,  1,  0,  6288774, -20905355},
    {2,  0, -1,  0,  1274027,  -3699111},
    {2,  0,  0,  0,   658314,  -2955968},
    {0,  0,  2,  0,   213618,   -569925},
    {0,  1,  0,  0,  -185116,     48888},
    {0,  0,  0,  2,  -114332,     -3149},
    {2,  0, -2,  0,    58793,    246158},
    {2, -1, -1,  0,    57066,   -152138},
    {2,  0,  1,  0,    53322,   -170733},
    {2, -1,  0,  0,    45758,   -204586},
    {0,  1, -1,  0,   -40923,   -129620},
    {1,  0,  0,  0,   -34720,    108743},
    {0,  1,  1,  0,   -30383,    104755},
    {2,  0,  0, -2,    15327,     10321},
    {0,  0,  1,  2,   -12528,         0},
    {0,  0,  1, -2,    10980,     79661},
    {4,  0, -1,  0,    10675,    -34782},
    {0,  0,  3,  0,    10034,    -23210},
    {4,  0, -2,  0,     8548,    -21636},
    {2,  1, -1,  0,    -7888,     24208},
    {2,  1,  0,  0,    -6766,     30824},
    {1,  0, -1,  0,    -5163,     -8379},
    {1,  1,  0,  0,     4987,    -16675},
    {2, -1,  1,  0,     4036,    -12831},
    {2,  0,  2,  0,     3994,    -10445},
    {4,  0,  0,  0,     3861,    -11650},
    {2,  0, -3,  0,     3665,     14403},
    {0,  1, -2,  0,    -2689,     -7003},
    {2,  0, -1,  2,    -2602,         0},
    {2, -1, -2,  0,     2390,     10056},
    {1,  0,  1,  0,    -2348,      6322},
    {2, -2,  0,  0,     2236,     -9884},
    {0,  1,  2,  0,    -2120,      5751},
    {0,  2,  0,  0,    -2069,         0},
    {2, -2, -1,  0,     2048,     -4950},
    {2,  0,  1, -2,    -1773,      4130},
    {2,  0,  0,  2,    -1595,         0},
    {4, -1, -1,  0,     1215,     -3958},
    {0,  0,  2,  2,    -1110,         0},
    {3,  0, -1,  0,     -892,      3258},
    {2,  1,  1,  0,     -810,      2616},
    {4, -1, -2,  0,      759,     -1897},
    {0,  2, -1,  0,     -713,     -2117},
    {2,  2, -1,  0,     -700,      2354},
    {2,  1, -2,  0,      691,         0},
    {2, -1,  0, -2,      596,         0},
    {4,  0,  1,  0,      549,     -1423},
    {0,  0,  4,  0,      537,     -1117},
    {4, -1,  0,  0,      520,     -1571},
    {1,  0, -2,  0,     -487,     -1739},
    {2,  1,  0, -2,     -399,         0},
    {0,  0,  2, -2,     -381,     -4421},
    {1,  1,  1,  0,      351,         0},
    {3,  0, -2,  0,     -340,         0},
    {4,  0, -3,  0,      330,         0},
    {2, -1,  2,  0,      327,         0},
    {0,  2,  1,  0,     -323,      1165},
    {1,  1, -1,  0,      299,         0},
    {2,  0,  3,  0,      294,         0},
    {2,  0, -1, -2,        0,      8752},
};

// Meeus table 47.B (Σb in 10^-6 °).
static const b_term_t B_TERMS[] = {
    {0,  0,  0,  1,  5128122},
    {0,  0,  1,  1,   280602},
    {0,  0,  1, -1,   277693},
    {2,  0,  0, -1,   173237},
    {2,  0, -1,  1,    55413},
    {2,  0, -1, -1,    46271},
    {2,  0,  0,  1,    32573},
    {0,  0,  2,  1,    17198},
    {2,  0,  1, -1,     9266},
    {0,  0,  2, -1,     8822},
    {2, -1,  0, -1,     8216},
    {2,  0, -2, -1,     4324},
    {2,  0,  1,  1,     4200},
    {2,  1,  0, -1,    -3359},
    {2, -1, -1,  1,     2463},
    {2, -1,  0,  1,     2211},
    {2, -1, -1, -1,     2065},
    {0,  1, -1, -1,    -1870},
    {4,  0, -1, -1,     1828},
    {0,  1,  0,  1,    -1794},
    {0,  0,  0,  3,    -1749},
    {0,  1, -1,  1,    -1565},
    {1,  0,  0,  1,    -1491},
    {0,  1,  1,  1,    -1475},
    {0,  1,  1, -1,    -1410},
    {0,  1,  0, -1,    -1344},
    {1,  0,  0, -1,    -1335},
    {0,  0,  3,  1,     1107},
    {4,  0,  0, -1,     1021},
    {4,  0, -1,  1,      833},
    {0,  0,  1, -3,      777},
    {4,  0, -2,  1,      671},
    {2,  0,  0, -3,      607},
    {2,  0,  2, -1,      596},
    {2, -1,  1, -1,      491},
    {2,  0, -2,  1,     -451},
    {0,  0,  3, -1,      439},
    {2,  0,  2,  1,      422},
    {2,  0, -3, -1,      421},
    {2,  1, -1,  1,     -366},
    {2,  1,  0,  1,     -351},
    {4,  0,  0,  1,      331},
    {2, -1,  1,  1,      315},
    {2, -2,  0, -1,      302},
    {0,  0,  1,  3,     -283},
    {2,  1,  1, -1,     -229},
    {1,  1,  0, -1,      223},
    {1,  1,  0,  1,      223},
    {0,  1, -2, -1,     -220},
    {2,  1, -1, -1,     -220},
    {1,  0,  1,  1,     -185},
    {2, -1, -2, -1,      181},
    {0,  1,  2,  1,     -177},
    {4,  0, -2, -1,      176},
    {4, -1, -1, -1,      166},
    {1,  0,  1, -1,     -164},
    {4,  0,  1, -1,      132},
    {1,  0, -1, -1,     -119},
    {4, -1,  0, -1,      115},
    {2, -2,  0,  1,      107},
};

#define LR_TERM_COUNT (sizeof(LR_TERMS) / sizeof(LR_TERMS[0]))
#define B_TERM_COUNT  (sizeof(B_TERMS)  / sizeof(B_TERMS[0]))

typedef struct {
    double ra_rad;       // geocentric right ascension
    double dec_rad;      // geocentric declination
    double dist_km;      // geocentric distance, km
    double app_long_rad; // apparent geocentric ecliptic longitude
    double app_lat_rad;  // apparent geocentric ecliptic latitude
    double obliquity_rad;
    double gmst_rad;     // Greenwich mean sidereal time at the instant
    double app_gst_rad;  // Greenwich apparent sidereal time
} lunar_geocentric_t;

// Compute geocentric lunar position and Greenwich apparent sidereal
// time at the given JD (Meeus ch. 47 + 12). Pure function: no globals
// touched. Output filled into *p.
static void lunar_geocentric_at_jd(double jd, lunar_geocentric_t* p) {
    // T in Julian centuries since J2000.0 (Meeus ch. 22).
    double T  = (jd - 2451545.0) / 36525.0;
    double T2 = T * T;
    double T3 = T2 * T;
    double T4 = T3 * T;

    // Fundamental arguments (Meeus 47.1-47.5, 47.6 etc., all in deg).
    double Lp = 218.3164477 + 481267.88123421 * T
              - 0.0015786   * T2
              + T3 / 538841.0
              - T4 / 65194000.0;                                // Moon's mean longitude
    double D  = 297.8501921 + 445267.1114034 * T
              - 0.0018819   * T2
              + T3 / 545868.0
              - T4 / 113065000.0;                               // mean elongation
    double M  = 357.5291092 + 35999.0502909   * T
              - 0.0001536   * T2
              + T3 / 24490000.0;                                // Sun's mean anomaly
    double Mp = 134.9633964 + 477198.8675055   * T
              + 0.0087414   * T2
              + T3 / 69699.0
              - T4 / 14712000.0;                                // Moon's mean anomaly
    double F  = 93.2720950  + 483202.0175233   * T
              - 0.0036539   * T2
              - T3 / 3526000.0
              + T4 / 863310000.0;                               // Moon's argument of latitude

    // Earth's orbital eccentricity factor E (Meeus 47.6). Multiplies
    // any term whose M coefficient is ±1 (E once) or ±2 (E^2).
    double E  = 1.0 - 0.002516 * T - 0.0000074 * T2;
    double E2 = E * E;

    double D_r  = D  * DEG_TO_RAD;
    double M_r  = M  * DEG_TO_RAD;
    double Mp_r = Mp * DEG_TO_RAD;
    double F_r  = F  * DEG_TO_RAD;

    // Σl, Σr (longitude in 10^-6 deg, distance in 10^-3 km).
    double sigma_l = 0.0, sigma_r = 0.0;
    for (size_t i = 0; i < LR_TERM_COUNT; i++) {
        const lr_term_t* t = &LR_TERMS[i];
        double arg = t->d * D_r + t->m * M_r + t->mp * Mp_r + t->f * F_r;
        double scale = 1.0;
        int am = t->m < 0 ? -t->m : t->m;
        if      (am == 1) scale = E;
        else if (am == 2) scale = E2;
        sigma_l += scale * t->coeff_l * sin(arg);
        sigma_r += scale * t->coeff_r * cos(arg);
    }

    // Σb (latitude in 10^-6 deg).
    double sigma_b = 0.0;
    for (size_t i = 0; i < B_TERM_COUNT; i++) {
        const b_term_t* t = &B_TERMS[i];
        double arg = t->d * D_r + t->m * M_r + t->mp * Mp_r + t->f * F_r;
        double scale = 1.0;
        int am = t->m < 0 ? -t->m : t->m;
        if      (am == 1) scale = E;
        else if (am == 2) scale = E2;
        sigma_b += scale * t->coeff_b * sin(arg);
    }

    // Additional Venus/Jupiter/parallactic terms (Meeus p. 338, the
    // A1/A2/A3 corrections). Small (~few hundredths of a degree on the
    // longitude residual) but cheap.
    double A1 = mod_deg(119.75 +    131.849 * T) * DEG_TO_RAD;
    double A2 = mod_deg( 53.09 + 479264.290 * T) * DEG_TO_RAD;
    double A3 = mod_deg(313.45 + 481266.484 * T) * DEG_TO_RAD;
    sigma_l += 3958.0 * sin(A1)
             + 1962.0 * sin(Lp * DEG_TO_RAD - F_r)
             +  318.0 * sin(A2);
    sigma_b += -2235.0 * sin(Lp * DEG_TO_RAD)
             +   382.0 * sin(A3)
             +   175.0 * sin(A1 - F_r)
             +   175.0 * sin(A1 + F_r)
             +   127.0 * sin(Lp * DEG_TO_RAD - Mp_r)
             -   115.0 * sin(Lp * DEG_TO_RAD + Mp_r);

    // Geocentric ecliptic longitude / latitude (deg) and distance (km).
    double lambda = Lp + sigma_l / 1000000.0;
    double beta   =      sigma_b / 1000000.0;
    double Delta  = 385000.56 + sigma_r / 1000.0;

    // Nutation in longitude (Meeus eq. 22.1, low-precision form).
    double Omega    = 125.04452 - 1934.136261 * T;
    double L_sun    = 280.4665  +  36000.7698  * T;
    double Lp_for_n = Lp;       // already includes ΔL through sigma_l? No —
                                // Meeus eq. 22.1 uses *mean* longitudes,
                                // i.e. Lp without the perturbations.
    double dPsi_arcsec = -17.20 * sin(Omega    * DEG_TO_RAD)
                        -  1.32 * sin(2.0 * L_sun * DEG_TO_RAD)
                        -  0.23 * sin(2.0 * Lp_for_n * DEG_TO_RAD)
                        +  0.21 * sin(2.0 * Omega * DEG_TO_RAD);
    double dEps_arcsec =   9.20 * cos(Omega    * DEG_TO_RAD)
                        +  0.57 * cos(2.0 * L_sun * DEG_TO_RAD)
                        +  0.10 * cos(2.0 * Lp_for_n * DEG_TO_RAD)
                        -  0.09 * cos(2.0 * Omega * DEG_TO_RAD);
    double dPsi_deg = dPsi_arcsec / 3600.0;
    double dEps_deg = dEps_arcsec / 3600.0;

    // Apparent longitude (add nutation in longitude — Meeus p. 144).
    double app_lambda = lambda + dPsi_deg;

    // Mean obliquity (Meeus 22.2, arcseconds form).
    double eps0_arcsec = 84381.448
                       + (-46.8150
                          + (-0.00059
                             +  0.001813 * T) * T) * T;
    double eps0_deg = eps0_arcsec / 3600.0;
    double eps_deg  = eps0_deg + dEps_deg;
    double eps_rad  = eps_deg * DEG_TO_RAD;

    double lam_rad = app_lambda * DEG_TO_RAD;
    double bet_rad = beta * DEG_TO_RAD;

    // Convert ecliptic -> equatorial (Meeus 13.3, 13.4).
    double sin_b = sin(bet_rad), cos_b = cos(bet_rad);
    double sin_l = sin(lam_rad), cos_l = cos(lam_rad);
    double sin_e = sin(eps_rad), cos_e = cos(eps_rad);

    double ra  = atan2(sin_l * cos_e - tan(bet_rad) * sin_e, cos_l);
    double dec = asin(sin_b * cos_e + cos_b * sin_e * sin_l);

    // Greenwich mean sidereal time at jd (Meeus eq. 12.4, deg form).
    double gmst_deg = 280.46061837
                    + 360.98564736629 * (jd - 2451545.0)
                    + 0.000387933 * T2
                    - T3 / 38710000.0;
    double app_gst_deg = gmst_deg + dPsi_deg * cos(eps_rad);
    gmst_deg     = mod_deg(gmst_deg);
    app_gst_deg  = mod_deg(app_gst_deg);

    p->ra_rad        = ra < 0 ? ra + 2.0 * M_PI : ra;
    p->dec_rad       = dec;
    p->dist_km       = Delta;
    p->app_long_rad  = lam_rad;
    p->app_lat_rad   = bet_rad;
    p->obliquity_rad = eps_rad;
    p->gmst_rad      = gmst_deg    * DEG_TO_RAD;
    p->app_gst_rad   = app_gst_deg * DEG_TO_RAD;
}

// ---- Topocentric correction (Meeus ch. 11 + 13) -------------------
//
// At observer geodetic latitude phi and elevation 0 (we ignore camera
// altitude — adds <1 arcsec to the parallax shift, well below the
// 2-min FR-4.5 budget), compute the geocentric coordinates of the
// observer normalised to Earth's equatorial radius:
//
//   rho_sin_phi' = (1 - f) * sin(u) + (h/a) * sin(phi)
//   rho_cos_phi' = cos(u)            + (h/a) * cos(phi)
//
// where tan(u) = (1 - f) * tan(phi). With h = 0 the (h/a) terms drop
// out. This is Meeus eq. 11.1.
typedef struct {
    double rho_sin_phi_p;
    double rho_cos_phi_p;
} observer_geo_t;

static observer_geo_t observer_geocentric(double lat_rad) {
    double u = atan((1.0 - WGS84_FLATTENING) * tan(lat_rad));
    observer_geo_t o = {
        .rho_sin_phi_p = (1.0 - WGS84_FLATTENING) * sin(u),
        .rho_cos_phi_p = cos(u)
    };
    // Edge case: at the geographic poles tan(lat) blows up but
    // |sin(u)| -> 1, |cos(u)| -> 0 by L'Hopital — atan handles it.
    return o;
}

// Compute the Moon's topocentric altitude (radians) at the given JD
// for an observer at (lat_rad, lon_rad). Positive = above horizon.
//
// Meeus ch. 13 (geocentric -> topocentric via parallax in altitude).
// The horizontal parallax pi is sin(pi) = 6378.14 / Delta; we use the
// approximate small-angle form sin(pi) ≈ 8.794" / Delta_AU but with
// Delta in km that becomes sin(pi) = 6378.14 / Delta which is Meeus
// p. 337's exact form for the lunar problem.
//
// Returned alongside the topocentric altitude is the topocentric
// horizontal parallax (in radians) — needed by the rise/set h0
// formula.
static void lunar_topocentric_alt(double jd,
                                  double lat_rad, double lon_rad,
                                  observer_geo_t obs,
                                  double* alt_rad,
                                  double* hp_rad) {
    lunar_geocentric_t g;
    lunar_geocentric_at_jd(jd, &g);

    // Horizontal parallax of the Moon (Meeus eq. 47.7).
    double sin_pi = 6378.14 / g.dist_km;
    double pi_rad = asin(sin_pi);
    if (hp_rad) *hp_rad = pi_rad;

    // Local hour angle = apparent GST + observer longitude - RA.
    // Meeus uses west-positive longitude; we use east-positive (the
    // ACAP convention), so we ADD lon_rad.
    double H = g.app_gst_rad + lon_rad - g.ra_rad;

    // Apply parallax (Meeus eq. 40.6/40.7).
    double cos_dec = cos(g.dec_rad);
    double sin_H   = sin(H);
    double cos_H   = cos(H);

    double dRA = atan2(-obs.rho_cos_phi_p * sin_pi * sin_H,
                        cos_dec - obs.rho_cos_phi_p * sin_pi * cos_H);
    double dec_p = atan2((sin(g.dec_rad) - obs.rho_sin_phi_p * sin_pi) * cos(dRA),
                         cos_dec - obs.rho_cos_phi_p * sin_pi * cos_H);
    double H_p = H - dRA;

    // Topocentric altitude (Meeus eq. 13.6).
    double sin_alt = sin(lat_rad) * sin(dec_p)
                   + cos(lat_rad) * cos(dec_p) * cos(H_p);
    if (sin_alt >  1.0) sin_alt =  1.0;
    if (sin_alt < -1.0) sin_alt = -1.0;
    *alt_rad = asin(sin_alt);
}

// Compute the Moon's local hour angle (radians, in (-pi, pi]) from
// observer longitude. Used to find transit/anti-transit (zero-
// crossings of H and H ± pi). Geocentric (parallax has negligible
// effect on transit time).
static double lunar_local_hour_angle(double jd, double lon_rad) {
    lunar_geocentric_t g;
    lunar_geocentric_at_jd(jd, &g);
    double H = g.app_gst_rad + lon_rad - g.ra_rad;
    // Reduce to (-pi, pi].
    H = fmod(H, 2.0 * M_PI);
    if (H >  M_PI) H -= 2.0 * M_PI;
    if (H < -M_PI) H += 2.0 * M_PI;
    return H;
}

// ---- Rise/set/transit search --------------------------------------

// Linear interpolation between (x0,y0) and (x1,y1) at y = 0.
// Caller must ensure y0 and y1 straddle zero (different signs).
static double linear_root(double x0, double y0, double x1, double y1) {
    return x0 - y0 * (x1 - x0) / (y1 - y0);
}

// Refine a horizon-crossing time using one Newton-style iteration:
// recompute altitude at the linear-interpolation estimate, and re-
// linear-interpolate against the closer of the two original brackets.
// One iteration is enough for sub-second convergence for the lunar
// rise/set rate (~0.25 deg/min near the horizon); two would be wasted
// CPU on the camera.
static double refine_rise_set(double lat_rad, double lon_rad,
                              observer_geo_t obs,
                              double jd0, double alt0,
                              double jd1, double alt1,
                              double h0_target_rad) {
    double y0 = alt0 - h0_target_rad;
    double y1 = alt1 - h0_target_rad;
    double jd_est = linear_root(jd0, y0, jd1, y1);

    // Recompute altitude at the estimate; HP may shift slightly so
    // recompute the target threshold too.
    double alt_est, hp_est;
    lunar_topocentric_alt(jd_est, lat_rad, lon_rad, obs, &alt_est, &hp_est);
    double h0_est = -0.2725 * hp_est - STANDARD_REFRACTION_DEG * DEG_TO_RAD;
    double y_est = alt_est - h0_est;

    // Pick the bracket end that has the opposite sign and re-fit.
    if ((y0 < 0.0) != (y_est < 0.0)) {
        // root between jd0 and jd_est
        return linear_root(jd0, y0, jd_est, y_est);
    } else {
        return linear_root(jd_est, y_est, jd1, y1);
    }
}

// Constrain a candidate UTC instant to be inside the day window
// [day_start_unix, day_start_unix + 86400) -- returns LUNAR_NO_EVENT
// if outside, otherwise rounded time_t.
static time_t maybe_event_in_window(double t_unix,
                                    int64_t day_start_unix) {
    int64_t t_secs = (int64_t)llround(t_unix);
    if (t_secs < day_start_unix || t_secs >= day_start_unix + 86400) {
        return LUNAR_NO_EVENT;
    }
    return (time_t)t_secs;
}

// Pick the transit/anti-transit candidate inside the UTC day window;
// fall back to the nearest one (by absolute distance to UTC noon of
// the input day) if none is in-window. The header guarantees a usable
// value within ±13 h of noon.
static time_t pick_transit_candidate(const double* candidates,
                                     int n,
                                     int64_t day_start_unix) {
    int64_t noon_unix = day_start_unix + 43200;
    // First pass: prefer in-window.
    double best_in = 0.0;
    int    have_in = 0;
    for (int i = 0; i < n; i++) {
        int64_t t = (int64_t)llround(candidates[i]);
        if (t >= day_start_unix && t < day_start_unix + 86400) {
            // Among multiple in-window hits (rare — would require a
            // ~24h spacing collision), prefer the one closest to noon.
            int64_t d = t > noon_unix ? t - noon_unix : noon_unix - t;
            if (!have_in || d < (int64_t)llround(
                    fabs(best_in - (double)noon_unix))) {
                best_in = candidates[i];
                have_in = 1;
            }
        }
    }
    if (have_in) return (time_t)llround(best_in);

    // Second pass: nearest to noon, regardless of window.
    int    best_i = -1;
    double best_d = 0.0;
    for (int i = 0; i < n; i++) {
        double d = fabs(candidates[i] - (double)noon_unix);
        if (best_i < 0 || d < best_d) { best_i = i; best_d = d; }
    }
    if (best_i < 0) return LUNAR_NO_EVENT; // unreachable in normal use
    return (time_t)llround(candidates[best_i]);
}

// ---- Public API: lunar_compute_daily ------------------------------

int lunar_compute_daily(double lat, double lon,
                        int year, int month, int day,
                        lunar_events_t* out) {
    if (!out) return -1;
    out->moonrise          = LUNAR_NO_EVENT;
    out->moonset           = LUNAR_NO_EVENT;
    out->lunar_transit     = LUNAR_NO_EVENT;
    out->lunar_anti_transit = LUNAR_NO_EVENT;

    if (lat < -90.0 || lat > 90.0)   return -1;
    if (lon < -180.0 || lon > 180.0) return -1;
    if (!valid_date(year, month, day)) return -1;

    double lat_rad = lat * DEG_TO_RAD;
    double lon_rad = lon * DEG_TO_RAD;
    observer_geo_t obs = observer_geocentric(lat_rad);

    int64_t day_start_unix = days_from_civil(year, (unsigned)month, (unsigned)day)
                           * (int64_t)86400;

    // Sample altitude at hourly intervals from -2h to +26h around UTC
    // midnight of the input date — 29 samples covering [-2..+26]. The
    // ±2h padding catches rises/sets that straddle the day boundary.
    enum { N_SAMPLES = 29 };
    double t_unix[N_SAMPLES];
    double alt[N_SAMPLES];
    double hp [N_SAMPLES];
    for (int i = 0; i < N_SAMPLES; i++) {
        t_unix[i] = (double)day_start_unix + (i - 2) * 3600.0;
        double jd_i = jd_from_unix(t_unix[i]);
        lunar_topocentric_alt(jd_i, lat_rad, lon_rad, obs,
                              &alt[i], &hp[i]);
    }

    // Identify rise/set zero-crossings of (alt - h0). h0 is altitude-
    // dependent only through HP, which varies <1% across a day, so
    // using the per-sample HP is more than precise enough.
    int rise_found = 0, set_found = 0;
    for (int i = 0; i < N_SAMPLES - 1; i++) {
        double h0_i = -0.2725 * hp[i]
                    - STANDARD_REFRACTION_DEG * DEG_TO_RAD;
        double h0_j = -0.2725 * hp[i + 1]
                    - STANDARD_REFRACTION_DEG * DEG_TO_RAD;
        double y_i = alt[i]     - h0_i;
        double y_j = alt[i + 1] - h0_j;
        if ((y_i < 0.0) == (y_j < 0.0)) continue;  // same sign, no crossing

        // Linear bracket then one-step refinement.
        double jd0 = jd_from_unix(t_unix[i]);
        double jd1 = jd_from_unix(t_unix[i + 1]);
        double jd_root = refine_rise_set(lat_rad, lon_rad, obs,
                                         jd0, alt[i],
                                         jd1, alt[i + 1],
                                         -0.2725 * 0.5 * (hp[i] + hp[i+1])
                                            - STANDARD_REFRACTION_DEG * DEG_TO_RAD);
        double t_root = unix_from_jd(jd_root);

        // Rise: altitude going from below to above => y increasing.
        if (y_i < 0.0 && y_j > 0.0) {
            if (!rise_found) {
                time_t cand = maybe_event_in_window(t_root, day_start_unix);
                if (cand != LUNAR_NO_EVENT) {
                    out->moonrise = cand;
                    rise_found = 1;
                }
            }
        } else {  // y_i > 0, y_j < 0
            if (!set_found) {
                time_t cand = maybe_event_in_window(t_root, day_start_unix);
                if (cand != LUNAR_NO_EVENT) {
                    out->moonset = cand;
                    set_found = 1;
                }
            }
        }
        if (rise_found && set_found) break;
    }

    // Transit and anti-transit: track sign changes of the local hour
    // angle H. Transit happens when H goes from negative to positive
    // (Moon crosses upper meridian, increasing); anti-transit when H
    // crosses ±π (we detect this by tracking sign changes of (H - π)
    // wrapped, which is equivalent to detecting a flip in sign of
    // sin(H/2) -- or, more pragmatically, sample over a wider band
    // and detect transitions of (H - target) for target = 0 and ±π).
    //
    // Simpler implementation: sample H at hourly intervals, watch for
    // the +π -> -π wrap as the upper-meridian crossing (transit), and
    // the 0 -> 0 wrap... actually no, easier still: H continuously
    // decreases at ~14.5°/h after subtracting the +2π wraps. Treat
    // H_unwrapped as a monotone function and find times where
    // H_unwrapped mod 2π equals 0 (transit) or ±π (anti-transit).

    // Pre-compute hourly hour angles.
    double H_raw[N_SAMPLES];
    for (int i = 0; i < N_SAMPLES; i++) {
        H_raw[i] = lunar_local_hour_angle(jd_from_unix(t_unix[i]),
                                          lon_rad);
    }
    // Unwrap to a monotonically-increasing sequence. Sidereal
    // rotation sends H increasing at 15°/h while the Moon's RA
    // increases at ~0.5°/h, net H increases at ~14.5°/h. The mod-2π
    // wrap from +π to -π is what we undo here.
    double H[N_SAMPLES];
    H[0] = H_raw[0];
    for (int i = 1; i < N_SAMPLES; i++) {
        double prev = H[i - 1];
        double cur  = H_raw[i];
        while (cur - prev < -M_PI) cur += 2.0 * M_PI;
        while (cur - prev >  M_PI) cur -= 2.0 * M_PI;
        H[i] = cur;
    }

    // Find crossings of multiples of π. transit at H = 2kπ;
    // anti-transit at H = (2k+1)π.
    double transit_cands[4]; int n_transit = 0;
    double anti_cands   [4]; int n_anti    = 0;

    for (int i = 0; i < N_SAMPLES - 1; i++) {
        double a = H[i], b = H[i + 1];
        // Transit candidates: nearest multiple of 2π that lies in
        // [min(a,b), max(a,b)].
        double lo = a < b ? a : b;
        double hi = a < b ? b : a;
        // Walk through multiples of π in [lo, hi].
        long k_lo = (long)floor(lo / M_PI);
        long k_hi = (long)ceil (hi / M_PI);
        for (long k = k_lo; k <= k_hi; k++) {
            double target = (double)k * M_PI;
            if (target < lo || target > hi) continue;
            // Linear interpolation for the crossing time.
            if (a == b) continue;  // pathological, skip
            double frac = (target - a) / (b - a);
            double t_cross = t_unix[i] + frac * (t_unix[i + 1] - t_unix[i]);

            // One Newton refinement: recompute H at t_cross,
            // re-interpolate. The drift of H is dominated by Earth
            // rotation, so even without refinement we'd be within a
            // few seconds — but the recompute also averages out the
            // small RA drift from lunar motion.
            double H_mid = lunar_local_hour_angle(jd_from_unix(t_cross),
                                                  lon_rad);
            // Find the nearest integer multiple of π to H_mid (the
            // one we were aiming for) and refine.
            double target_local = (double)k * M_PI;
            // Wrap H_mid to within π of target_local.
            while (H_mid - target_local >  M_PI) H_mid -= 2.0 * M_PI;
            while (H_mid - target_local < -M_PI) H_mid += 2.0 * M_PI;
            double y_a = a - target_local;
            double y_m = H_mid - target_local;
            // refine using the bracket whose y has opposite sign.
            double t_refined;
            if ((y_a < 0.0) != (y_m < 0.0) && t_cross != t_unix[i]) {
                t_refined = t_unix[i] + (-y_a) * (t_cross - t_unix[i])
                                       / (y_m - y_a);
            } else {
                double y_b = b - target_local;
                if ((y_m < 0.0) != (y_b < 0.0) && t_unix[i+1] != t_cross) {
                    t_refined = t_cross + (-y_m) * (t_unix[i+1] - t_cross)
                                          / (y_b - y_m);
                } else {
                    t_refined = t_cross;
                }
            }

            if (k % 2 == 0) {
                if (n_transit < 4) transit_cands[n_transit++] = t_refined;
            } else {
                if (n_anti    < 4) anti_cands   [n_anti++]    = t_refined;
            }
        }
    }

    out->lunar_transit      = pick_transit_candidate(transit_cands,
                                                     n_transit,
                                                     day_start_unix);
    out->lunar_anti_transit = pick_transit_candidate(anti_cands,
                                                     n_anti,
                                                     day_start_unix);

    return 0;
}

// ---- Illumination (Meeus ch. 48) ----------------------------------

// Compute the Sun's apparent geocentric ecliptic longitude (radians)
// at the given JD. We re-derive this here rather than expose solar.c's
// helper because solar.c parameterises by Gregorian date, and we want
// per-instant precision for phase-angle calculation.
static double solar_apparent_long_rad(double jd) {
    double T  = (jd - 2451545.0) / 36525.0;

    // Meeus 25.2 / 25.3.
    double L0 = 280.46646
              + (36000.76983 + 0.0003032 * T) * T;
    double M  = 357.52911
              + (35999.05029 - 0.0001537 * T) * T;
    double M_r = mod_deg(M) * DEG_TO_RAD;

    // Equation of center.
    double C = (1.914602 - (0.004817 + 0.000014 * T) * T) * sin(M_r)
             + (0.019993 - 0.000101 * T)               * sin(2.0 * M_r)
             +  0.000289                                * sin(3.0 * M_r);

    double trueL = L0 + C;
    double Omega = (125.04 - 1934.136 * T) * DEG_TO_RAD;
    double appL  = trueL - 0.00569 - 0.00478 * sin(Omega);
    return mod_deg(appL) * DEG_TO_RAD;
}

double lunar_illumination(time_t when) {
    if (when < 0) return -1.0;
    double jd = jd_from_unix((double)when);

    // Meeus eq. 48.2 / 48.3 — geocentric formulation.
    // Phase angle i: cos i = cos β * cos(λ - λ_sun) — where (λ, β) is
    // the Moon's apparent geocentric ecliptic position, λ_sun the
    // Sun's apparent geocentric longitude. The 2pp budget tolerates
    // ignoring the topocentric correction and the sun-moon distance
    // ratio (the full eq. 48.3 form differs from this simplified
    // cos(elongation) by <<1pp throughout the synodic month).
    lunar_geocentric_t g;
    lunar_geocentric_at_jd(jd, &g);
    double lam_sun = solar_apparent_long_rad(jd);

    double cos_psi = cos(g.app_lat_rad) * cos(g.app_long_rad - lam_sun);
    if (cos_psi >  1.0) cos_psi =  1.0;
    if (cos_psi < -1.0) cos_psi = -1.0;
    double psi = acos(cos_psi);  // elongation Moon-Sun

    // Phase angle i ≈ π - ψ (Meeus eq. 48.3 simplified for r≫Δ).
    double i_rad = M_PI - psi;
    double k = (1.0 + cos(i_rad)) / 2.0;
    if (k < 0.0) k = 0.0;
    if (k > 1.0) k = 1.0;
    return k;
}

// ---- Phases (Meeus ch. 49) ----------------------------------------
//
// Meeus eq. 49.1: JDE = 2451550.09766 + 29.530588861 k
//                     + 0.00015437 T^2 - 0.000000150 T^3 + ...
// where T = k / 1236.85 and k is incremented by 0.25 per principal
// phase (k integer = new moon, k.25 = first quarter, k.5 = full,
// k.75 = last quarter).
//
// On top of the mean JDE we add the periodic correction series
// (Meeus tables 49.A — new/full moons — and 49.B — quarters, plus
// the planetary corrections). We use the truncated forms tabulated in
// the chapter, which deliver <2 min accuracy 1900-2100, well inside
// the 5-min FR-4.5 budget.

// Solve for the JDE of the phase identified by k_quarter
// (k_quarter = 4*k + phase_index, phase_index in {0..3}).
// Returns the JDE (TT-based; we treat it as UTC for the FR-4.5
// 5-min budget — ΔT is at most ~70 s in 2000-2100, an order of
// magnitude under our budget).
static double phase_jde(double k_quarter) {
    double k = k_quarter / 4.0;
    double T = k / 1236.85;
    double T2 = T * T, T3 = T2 * T, T4 = T3 * T;

    // Mean JDE (Meeus 49.1).
    double JDE = 2451550.09766
               + 29.530588861 * k
               + 0.00015437 * T2
               - 0.000000150 * T3
               + 0.00000000073 * T4;

    // Eccentricity factor E (Meeus p. 350, eq. 47.6 evaluated at the
    // T scale appropriate for the synodic-month progression).
    double E = 1.0 - 0.002516 * T - 0.0000074 * T2;

    // Sun's mean anomaly M, Moon's mean anomaly M', Moon's argument
    // of latitude F, longitude of ascending node Omega — all degrees
    // (Meeus eq. 49.2-49.5).
    double M  = 2.5534
              + 29.10535670 * k
              - 0.0000014   * T2
              - 0.00000011  * T3;
    double Mp = 201.5643
              + 385.81693528 * k
              + 0.0107582    * T2
              + 0.00001238   * T3
              - 0.000000058  * T4;
    double F  = 160.7108
              + 390.67050284 * k
              - 0.0016118    * T2
              - 0.00000227   * T3
              + 0.000000011  * T4;
    double Omega = 124.7746
                 -  1.56375588 * k
                 +  0.0020672  * T2
                 +  0.00000215 * T3;

    double Mr  = mod_deg(M)  * DEG_TO_RAD;
    double Mpr = mod_deg(Mp) * DEG_TO_RAD;
    double Fr  = mod_deg(F)  * DEG_TO_RAD;
    double Or  = mod_deg(Omega) * DEG_TO_RAD;

    double corr = 0.0;
    int phase_index = (int)llround(k_quarter - 4.0 * floor(k));
    if (phase_index >= 4) phase_index -= 4;
    if (phase_index < 0)  phase_index += 4;

    if (phase_index == 0 || phase_index == 2) {
        // New (0) or Full (2) moon — Meeus table 49.A on pp. 351.
        // Sign of the Mp term flips between new and full.
        double s = (phase_index == 0) ? -0.40720 : -0.40614;
        corr += s             * sin(Mpr)
              + 0.17241 * E   * sin(Mr)
              + 0.01608       * sin(2.0 * Mpr)
              + 0.01039       * sin(2.0 * Fr)
              + 0.00739 * E   * sin(Mpr - Mr)
              - 0.00514 * E   * sin(Mpr + Mr)
              + 0.00208 * E*E * sin(2.0 * Mr)
              - 0.00111       * sin(Mpr - 2.0 * Fr)
              - 0.00057       * sin(Mpr + 2.0 * Fr)
              + 0.00056 * E   * sin(2.0 * Mpr + Mr)
              - 0.00042       * sin(3.0 * Mpr)
              + 0.00042 * E   * sin(Mr + 2.0 * Fr)
              + 0.00038 * E   * sin(Mr - 2.0 * Fr)
              - 0.00024 * E   * sin(2.0 * Mpr - Mr)
              - 0.00017       * sin(Or)
              - 0.00007       * sin(Mpr + 2.0 * Mr)
              + 0.00004       * sin(2.0 * Mpr - 2.0 * Fr)
              + 0.00004       * sin(3.0 * Mr)
              + 0.00003       * sin(Mpr + Mr - 2.0 * Fr)
              + 0.00003       * sin(2.0 * Mpr + 2.0 * Fr)
              - 0.00003       * sin(Mpr + Mr + 2.0 * Fr)
              + 0.00003       * sin(Mpr - Mr + 2.0 * Fr)
              - 0.00002       * sin(Mpr - Mr - 2.0 * Fr)
              - 0.00002       * sin(3.0 * Mpr + Mr)
              + 0.00002       * sin(4.0 * Mpr);
    } else {
        // First (1) or Last (3) quarter — Meeus table 49.A bottom
        // section. The W correction is added/subtracted depending on
        // which quarter.
        corr += -0.62801       * sin(Mpr)
              +  0.17172 * E   * sin(Mr)
              -  0.01183 * E   * sin(Mpr + Mr)
              +  0.00862       * sin(2.0 * Mpr)
              +  0.00804       * sin(2.0 * Fr)
              +  0.00454 * E   * sin(Mpr - Mr)
              +  0.00204 * E*E * sin(2.0 * Mr)
              -  0.00180       * sin(Mpr - 2.0 * Fr)
              -  0.00070       * sin(Mpr + 2.0 * Fr)
              -  0.00040       * sin(3.0 * Mpr)
              -  0.00034 * E   * sin(2.0 * Mpr - Mr)
              +  0.00032 * E   * sin(Mr + 2.0 * Fr)
              +  0.00032 * E   * sin(Mr - 2.0 * Fr)
              -  0.00028 * E*E * sin(Mpr + 2.0 * Mr)
              +  0.00027 * E   * sin(2.0 * Mpr + Mr)
              -  0.00017       * sin(Or)
              -  0.00005       * sin(Mpr - Mr - 2.0 * Fr)
              +  0.00004       * sin(2.0 * Mpr + 2.0 * Fr)
              -  0.00004       * sin(Mpr + Mr + 2.0 * Fr)
              +  0.00004       * sin(Mpr - 2.0 * Mr)
              +  0.00003       * sin(Mpr + Mr - 2.0 * Fr)
              +  0.00003       * sin(3.0 * Mr)
              +  0.00002       * sin(2.0 * Mpr - 2.0 * Fr)
              +  0.00002       * sin(Mpr - Mr + 2.0 * Fr)
              -  0.00002       * sin(3.0 * Mpr + Mr);

        // W (Meeus p. 352).
        double W = 0.00306
                 - 0.00038 * E * cos(Mr)
                 + 0.00026     * cos(Mpr)
                 - 0.00002     * cos(Mpr - Mr)
                 + 0.00002     * cos(Mpr + Mr)
                 + 0.00002     * cos(2.0 * Fr);
        corr += (phase_index == 1 ? +W : -W);
    }

    // Planetary corrections (Meeus p. 352). Same for all four
    // phases.
    double A1  = mod_deg(299.77 +    0.107408 * k - 0.009173 * T2) * DEG_TO_RAD;
    double A2  = mod_deg(251.88 +    0.016321 * k)                 * DEG_TO_RAD;
    double A3  = mod_deg(251.83 +   26.651886 * k)                 * DEG_TO_RAD;
    double A4  = mod_deg(349.42 +   36.412478 * k)                 * DEG_TO_RAD;
    double A5  = mod_deg( 84.66 +   18.206239 * k)                 * DEG_TO_RAD;
    double A6  = mod_deg(141.74 +   53.303771 * k)                 * DEG_TO_RAD;
    double A7  = mod_deg(207.14 +    2.453732 * k)                 * DEG_TO_RAD;
    double A8  = mod_deg(154.84 +    7.306860 * k)                 * DEG_TO_RAD;
    double A9  = mod_deg( 34.52 +   27.261239 * k)                 * DEG_TO_RAD;
    double A10 = mod_deg(207.19 +    0.121824 * k)                 * DEG_TO_RAD;
    double A11 = mod_deg(291.34 +    1.844379 * k)                 * DEG_TO_RAD;
    double A12 = mod_deg(161.72 +   24.198154 * k)                 * DEG_TO_RAD;
    double A13 = mod_deg(239.56 +   25.513099 * k)                 * DEG_TO_RAD;
    double A14 = mod_deg(331.55 +    3.592518 * k)                 * DEG_TO_RAD;

    corr += 0.000325 * sin(A1)
          + 0.000165 * sin(A2)
          + 0.000164 * sin(A3)
          + 0.000126 * sin(A4)
          + 0.000110 * sin(A5)
          + 0.000062 * sin(A6)
          + 0.000060 * sin(A7)
          + 0.000056 * sin(A8)
          + 0.000047 * sin(A9)
          + 0.000042 * sin(A10)
          + 0.000040 * sin(A11)
          + 0.000037 * sin(A12)
          + 0.000035 * sin(A13)
          + 0.000023 * sin(A14);

    return JDE + corr;
}

int lunar_next_phase(time_t after, lunar_phase_t kind, time_t* out) {
    if (!out) return -1;
    if (kind != LUNAR_PHASE_NEW
     && kind != LUNAR_PHASE_FIRST_QUARTER
     && kind != LUNAR_PHASE_FULL
     && kind != LUNAR_PHASE_LAST_QUARTER) return -1;
    if (after < 0) return -1;

    // Convert `after` to JD, then approximate years from 2000.
    double jd_after = jd_from_unix((double)after);
    double years_from_2000 = (jd_after - 2451545.0) / 365.25;

    // Synodic months per year ≈ 12.3685 (Meeus p. 350).
    // k_continuous is the synodic-month index that puts us roughly at
    // the requested instant. We add an extra cycle ahead and walk
    // backward to find the smallest k_quarter whose JDE > jd_after.
    double k_approx = years_from_2000 * 12.3685;

    // Phase index (offset within a synodic cycle, in quarters).
    int phase_index = (int)kind;  // 0..3

    // Build k_quarter = 4*k + phase_index, where k is the synodic
    // cycle index. Start a few cycles ahead and decrement.
    long k_int = (long)floor(k_approx) + 2;
    long kq = 4 * k_int + phase_index;

    // Walk backward by 4 (one synodic month) at a time until we step
    // before `after`, then the previous step is the first one strictly
    // after.
    double jde_curr = phase_jde((double)kq);
    double t_curr   = unix_from_jd(jde_curr);
    while (t_curr > (double)after) {
        long kq_prev = kq - 4;
        double jde_prev = phase_jde((double)kq_prev);
        double t_prev   = unix_from_jd(jde_prev);
        if (t_prev <= (double)after) {
            // jde_curr is the answer.
            break;
        }
        kq      = kq_prev;
        jde_curr = jde_prev;
        t_curr  = t_prev;
    }
    // If t_curr is still <= after, walk forward.
    while (t_curr <= (double)after) {
        kq += 4;
        jde_curr = phase_jde((double)kq);
        t_curr   = unix_from_jd(jde_curr);
    }

    *out = (time_t)llround(t_curr);
    return 0;
}
