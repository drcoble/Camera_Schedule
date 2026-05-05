// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Host-side test fixture for app/src/astro/solar.c. Validates each
// USNO-derived sunrise/sunset reference against the Meeus
// implementation under FR-3.7's ±60 s tolerance at |lat| ≤ 60°.
//
// Build (under app/Makefile's `test` target): a single executable
// linked against solar.c + libm + standard C. No dependency on the
// ACAP framework, GLib, or any vendored Timelapse2 code — solar.c is
// intentionally pure for exactly this reason.
//
// Run: ./test_solar; non-zero exit on any fixture exceeding tolerance.

#define _GNU_SOURCE
#include "../../src/astro/solar.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// -----------------------------------------------------------------
// Fixture table.
//
// Source:    USNO Astronomical Applications API, RS_OneDay endpoint
//            <https://aa.usno.navy.mil/api/rstt/oneday>, apiversion
//            4.0.1. NOAA's solar calculator
//            <https://gml.noaa.gov/grad/solcalc/> is an accepted
//            secondary source — none of the entries below required
//            it.
// Retrieved: 2026-05-05.
//
// Convention. USNO returns rise/set as HH:MM in the requested local
// civil timezone (resolution ±30 s). expected_sunrise_utc /
// expected_sunset_utc are Unix epoch seconds at the START of the
// reported minute in UTC; the ±60 s test tolerance absorbs the ±30 s
// source quantization. Spot-check shown in the agent transcript:
// Atlanta 2026-12-21 sunrise = 07:38 EST = 12:38:00 UTC = 1797856680.
//
// Coverage (16 fixtures):
//   - Atlanta GA × 4   (M2 lab acceptance site; both solstices, plus
//                       a DST spring-forward / day-before pair)
//   - Equatorial × 3   (Quito, Singapore, Nairobi)
//   - Southern hem × 5 (Sydney, Cape Town, Buenos Aires, Ushuaia,
//                       Auckland)
//   - High latitude × 2 (Stockholm 59.3°N, Aberdeen 57.1°N)
//   - Non-Western longitudes × 2 (Tokyo, Madrid)
// -----------------------------------------------------------------

typedef struct {
    double      lat;
    double      lon;
    int         year, month, day;
    const char* tz;
    time_t      expected_sunrise_utc;
    time_t      expected_sunset_utc;
    const char* note;
} solar_fixture_t;

static const solar_fixture_t SOLAR_FIXTURES[] = {
    /* --- Atlanta, GA: M2 lab acceptance site (33.7490 N, 84.3880 W) --- */
    { 33.7490, -84.3880, 2026,  6, 21, "America/New_York",
      1782037620, 1782089460,
      "Atlanta summer solstice, EDT" },

    { 33.7490, -84.3880, 2026, 12, 21, "America/New_York",
      1797856680, 1797892380,
      "Atlanta winter solstice, EST" },

    { 33.7490, -84.3880, 2026,  3,  8, "America/New_York",
      1772971020, 1773013200,
      "Atlanta DST spring-forward day" },

    { 33.7490, -84.3880, 2026,  3,  7, "America/New_York",
      1772884680, 1772926740,
      "Atlanta day before DST, EST" },

    /* --- Equatorial: equation-of-time dominates --- */
    { -0.1807, -78.4678, 2026,  9, 23, "America/Guayaquil",
      1790161380, 1790204940,
      "Quito autumnal equinox (lat ~0)" },

    {  1.3521, 103.8198, 2026,  6, 21, "Asia/Singapore",
      1781996400, 1782040320,
      "Singapore June solstice (eq, eastern hem)" },

    { -1.2921,  36.8219, 2026,  3, 20, "Africa/Nairobi",
      1773977820, 1774021380,
      "Nairobi March equinox" },

    /* --- Southern hemisphere --- */
    { -33.8688, 151.2093, 2026,  6, 21, "Australia/Sydney",
      1781989200, 1782024840,
      "Sydney austral winter" },

    { -33.9249,  18.4241, 2026, 12, 21, "Africa/Johannesburg",
      1797823920, 1797875820,
      "Cape Town austral summer" },

    { -34.6037, -58.3816, 2026,  3, 20, "America/Argentina/Buenos_Aires",
      1774000620, 1774044300,
      "Buenos Aires March equinox" },

    { -54.8019, -68.3030, 2026, 12, 21, "America/Argentina/Ushuaia",
      1797839460, 1797901860,
      "Ushuaia austral summer (lat -54.8)" },

    { -36.8485, 174.7633, 2026, 12, 21, "Pacific/Auckland",
      1797785880, 1797838800,
      "Auckland NZDT austral summer" },

    /* --- High latitude (|lat| in [55, 60]) --- */
    {  59.3293,  18.0686, 2026,  6, 21, "Europe/Stockholm",
      1782005460, 1782072480,
      "Stockholm June solstice (lat 59.3)" },

    {  57.1497,  -2.0943, 2026,  6, 21, "Europe/London",
      1782011520, 1782076080,
      "Aberdeen, Scotland, June solstice (lat 57.1)" },

    /* --- Non-American longitudes --- */
    {  35.6762, 139.6503, 2026,  9, 23, "Asia/Tokyo",
      1790109000, 1790152680,
      "Tokyo autumnal equinox" },

    {  40.4168,  -3.7038, 2026,  3, 20, "Europe/Madrid",
      1773987480, 1774031220,
      "Madrid March equinox" },
};

#define SOLAR_FIXTURE_COUNT (sizeof(SOLAR_FIXTURES) / sizeof(SOLAR_FIXTURES[0]))

// Per FR-3.7: the routine SHALL be accurate to ±60 s for |lat| ≤ 60°.
// This is a strict bound — the test fails if any fixture exceeds it.
#define TOLERANCE_SECONDS 60

static const char* iso_utc(time_t t, char buf[32]) {
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

int main(void) {
    int passed = 0, failed = 0;
    long worst_err = 0;

    printf("test_solar: %zu fixtures, tolerance ±%d s (FR-3.7)\n\n",
           SOLAR_FIXTURE_COUNT, TOLERANCE_SECONDS);

    for (size_t i = 0; i < SOLAR_FIXTURE_COUNT; i++) {
        const solar_fixture_t* f = &SOLAR_FIXTURES[i];

        solar_events_t got = {0};
        int rc = solar_compute(f->lat, f->lon,
                               f->year, f->month, f->day,
                               SOLAR_ZENITH_SUNRISE_SUNSET,
                               &got);
        if (rc != 0) {
            printf("  [%2zu] %-44s  solar_compute returned %d\n",
                   i, f->note, rc);
            failed += 2;
            continue;
        }

        long sr_err = (long)got.sunrise - (long)f->expected_sunrise_utc;
        long ss_err = (long)got.sunset  - (long)f->expected_sunset_utc;

        int sr_ok = (sr_err > -TOLERANCE_SECONDS && sr_err < TOLERANCE_SECONDS);
        int ss_ok = (ss_err > -TOLERANCE_SECONDS && ss_err < TOLERANCE_SECONDS);

        if (sr_ok) passed++; else failed++;
        if (ss_ok) passed++; else failed++;

        long worst_this = labs(sr_err) > labs(ss_err) ? labs(sr_err) : labs(ss_err);
        if (worst_this > worst_err) worst_err = worst_this;

        printf("  [%2zu] %-44s  sr=%+5lds %s  ss=%+5lds %s\n",
               i, f->note,
               sr_err, sr_ok ? "PASS" : "FAIL",
               ss_err, ss_ok ? "PASS" : "FAIL");

        if (!sr_ok || !ss_ok) {
            char b1[32], b2[32];
            printf("       computed sunrise %s, expected %s\n",
                   iso_utc(got.sunrise, b1),
                   iso_utc(f->expected_sunrise_utc, b2));
            printf("       computed sunset  %s, expected %s\n",
                   iso_utc(got.sunset,  b1),
                   iso_utc(f->expected_sunset_utc,  b2));
        }
    }

    printf("\n%d passed, %d failed (worst error %ld s, budget %d s)\n",
           passed, failed, worst_err, TOLERANCE_SECONDS);
    return failed == 0 ? 0 : 1;
}
