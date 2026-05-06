// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Host-side test fixture for app/src/astro/solar.c. Validates each
// USNO-derived sunrise / sunset reference against the Meeus
// implementation under FR-3.7's tolerances:
//
//   |lat| <= 60°       -> ±60 s
//   |lat| <= 66.5°     -> ±5 min
//
// Polar fixtures (|lat| > 66.5° during local night-of-the-pole) test
// the SOLAR_NO_EVENT branch instead of a numeric tolerance.
//
// Each fixture also asserts the structural invariants that hold for
// every input: solar_noon is always non-SOLAR_NO_EVENT, and
// solar_midnight == solar_noon - 43200 by construction.
//
// Build (under app/Makefile's `test` target): a single executable
// linked against solar.c + libm + standard C. No dependency on the
// ACAP framework, GLib, or any vendored Timelapse2 code.
//
// Run: ./test_solar; non-zero exit on any fixture failure.

#define _GNU_SOURCE
#include "../../src/astro/solar.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// FR-3.7 tier-1: |lat| <= 60°
#define TOLERANCE_TIER1_SECONDS 60
// FR-3.7 tier-2: 60° < |lat| <= 66.5°
#define TOLERANCE_TIER2_SECONDS 300

typedef struct {
    double      lat;
    double      lon;
    int         year, month, day;
    const char* tz;                       // documentation only — solar_compute is tz-pure
    double      zenith_deg;               // SOLAR_ZENITH_*
    time_t      expected_morning_utc;     // sunrise / dawn (or SOLAR_NO_EVENT)
    time_t      expected_evening_utc;     // sunset  / dusk (or SOLAR_NO_EVENT)
    int         tolerance_seconds;
    const char* note;
} solar_fixture_t;

// -----------------------------------------------------------------
// Standard sunrise / sunset fixtures (FR-3.1, FR-3.7 tier-1).
//
// Source:    USNO Astronomical Applications API, RS_OneDay endpoint
//            <https://aa.usno.navy.mil/api/rstt/oneday>, apiversion
//            4.0.1.
// Retrieved: 2026-05-05 (initial 16) + 2026-05-06 (Atlanta fall-back
//            and polar entries).
//
// Convention. USNO returns rise/set as HH:MM in the requested local
// civil timezone (resolution ±30 s). expected_*_utc fields are Unix
// epoch seconds at the START of the reported minute in UTC; the ±60 s
// tier-1 tolerance absorbs the ±30 s source quantization. Polar
// fixtures use SOLAR_NO_EVENT for both morning and evening, validating
// the FR-3.8 sentinel branch.
// -----------------------------------------------------------------

static const solar_fixture_t SOLAR_FIXTURES[] = {
    /* --- Atlanta, GA: M2 lab acceptance site (33.7490 N, 84.3880 W) --- */
    { 33.7490, -84.3880, 2026,  6, 21, "America/New_York",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1782037620, 1782089460, TOLERANCE_TIER1_SECONDS,
      "Atlanta summer solstice, EDT" },

    { 33.7490, -84.3880, 2026, 12, 21, "America/New_York",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1797856680, 1797892380, TOLERANCE_TIER1_SECONDS,
      "Atlanta winter solstice, EST" },

    { 33.7490, -84.3880, 2026,  3,  8, "America/New_York",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1772971020, 1773013200, TOLERANCE_TIER1_SECONDS,
      "Atlanta DST spring-forward day" },

    { 33.7490, -84.3880, 2026,  3,  7, "America/New_York",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1772884680, 1772926740, TOLERANCE_TIER1_SECONDS,
      "Atlanta day before DST, EST" },

    { 33.7490, -84.3880, 2026, 11,  1, "America/New_York",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1793534220, 1793573100, TOLERANCE_TIER1_SECONDS,
      "Atlanta DST fall-back day, EST (clocks back 02:00 -> 01:00)" },

    /* --- Equatorial: equation-of-time dominates --- */
    { -0.1807, -78.4678, 2026,  9, 23, "America/Guayaquil",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1790161380, 1790204940, TOLERANCE_TIER1_SECONDS,
      "Quito autumnal equinox (lat ~0)" },

    {  1.3521, 103.8198, 2026,  6, 21, "Asia/Singapore",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1781996400, 1782040320, TOLERANCE_TIER1_SECONDS,
      "Singapore June solstice (eq, eastern hem)" },

    { -1.2921,  36.8219, 2026,  3, 20, "Africa/Nairobi",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1773977820, 1774021380, TOLERANCE_TIER1_SECONDS,
      "Nairobi March equinox" },

    /* --- Southern hemisphere --- */
    { -33.8688, 151.2093, 2026,  6, 21, "Australia/Sydney",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1781989200, 1782024840, TOLERANCE_TIER1_SECONDS,
      "Sydney austral winter" },

    { -33.9249,  18.4241, 2026, 12, 21, "Africa/Johannesburg",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1797823920, 1797875820, TOLERANCE_TIER1_SECONDS,
      "Cape Town austral summer" },

    { -34.6037, -58.3816, 2026,  3, 20, "America/Argentina/Buenos_Aires",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1774000620, 1774044300, TOLERANCE_TIER1_SECONDS,
      "Buenos Aires March equinox" },

    { -54.8019, -68.3030, 2026, 12, 21, "America/Argentina/Ushuaia",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1797839460, 1797901860, TOLERANCE_TIER1_SECONDS,
      "Ushuaia austral summer (lat -54.8)" },

    { -36.8485, 174.7633, 2026, 12, 21, "Pacific/Auckland",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1797785880, 1797838800, TOLERANCE_TIER1_SECONDS,
      "Auckland NZDT austral summer" },

    /* --- High latitude (|lat| in [55, 60], still tier-1) --- */
    {  59.3293,  18.0686, 2026,  6, 21, "Europe/Stockholm",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1782005460, 1782072480, TOLERANCE_TIER1_SECONDS,
      "Stockholm June solstice (lat 59.3)" },

    {  57.1497,  -2.0943, 2026,  6, 21, "Europe/London",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1782011520, 1782076080, TOLERANCE_TIER1_SECONDS,
      "Aberdeen, Scotland, June solstice (lat 57.1)" },

    /* --- Non-American longitudes --- */
    {  35.6762, 139.6503, 2026,  9, 23, "Asia/Tokyo",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1790109000, 1790152680, TOLERANCE_TIER1_SECONDS,
      "Tokyo autumnal equinox" },

    {  40.4168,  -3.7038, 2026,  3, 20, "Europe/Madrid",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      1773987480, 1774031220, TOLERANCE_TIER1_SECONDS,
      "Madrid March equinox" },

    /* --- Polar: FR-3.8 SOLAR_NO_EVENT verification (M3) ---
     * Both fixtures are above 66.5° lat at the December solstice.
     * USNO's RS_OneDay returns the phenomenon "Object continuously
     * below the Horizon" — i.e. the sun never reaches the standard
     * sunrise/sunset zenith of 90.833°. The Meeus implementation
     * MUST return SOLAR_NO_EVENT for both sunrise and sunset and a
     * valid (non-SOLAR_NO_EVENT) solar_noon and solar_midnight.
     * The tolerance field is unused for SOLAR_NO_EVENT comparisons.
     */
    { 70.0000,  25.0000, 2026, 12, 21, "Europe/Berlin (lon 25E proxy)",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      SOLAR_NO_EVENT, SOLAR_NO_EVENT, 0,
      "Polar night at 70 N on December solstice (USNO: continuously below horizon)" },

    { 78.0000,  15.0000, 2026, 12, 21, "Europe/Oslo (lon 15E proxy)",
      SOLAR_ZENITH_SUNRISE_SUNSET,
      SOLAR_NO_EVENT, SOLAR_NO_EVENT, 0,
      "Polar night at 78 N on December solstice (Svalbard latitude)" },
};

#define SOLAR_FIXTURE_COUNT (sizeof(SOLAR_FIXTURES) / sizeof(SOLAR_FIXTURES[0]))

// ---- Helpers --------------------------------------------------------

static const char* iso_utc(time_t t, char buf[32]) {
    if (t == SOLAR_NO_EVENT) { snprintf(buf, 32, "NO_EVENT"); return buf; }
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// Returns 1 if the comparison passes, 0 otherwise. Sets *err to the
// signed error in seconds (or 0 for SOLAR_NO_EVENT cases).
static int compare_event(const char* label,
                         time_t got, time_t expected, int tolerance,
                         long* err_out, char* msg, size_t msg_sz) {
    *err_out = 0;
    if (expected == SOLAR_NO_EVENT) {
        if (got == SOLAR_NO_EVENT) {
            snprintf(msg, msg_sz, "%-3s NO_EVENT (expected) PASS", label);
            return 1;
        }
        char b[32];
        snprintf(msg, msg_sz, "%-3s expected NO_EVENT, got %s FAIL",
                 label, iso_utc(got, b));
        return 0;
    }
    if (got == SOLAR_NO_EVENT) {
        char b[32];
        snprintf(msg, msg_sz, "%-3s expected %s, got NO_EVENT FAIL",
                 label, iso_utc(expected, b));
        return 0;
    }
    long err = (long)got - (long)expected;
    *err_out = err;
    int ok = (err > -tolerance && err < tolerance);
    snprintf(msg, msg_sz, "%-3s err=%+5lds %s", label, err, ok ? "PASS" : "FAIL");
    return ok;
}

// ---- Test runner ----------------------------------------------------

int main(void) {
    int passed = 0, failed = 0;
    long worst_tier1 = 0, worst_tier2 = 0;

    printf("test_solar: %zu fixtures (FR-3.7: ±%ds tier-1, ±%ds tier-2;"
           " polar fixtures verify FR-3.8 SOLAR_NO_EVENT)\n\n",
           SOLAR_FIXTURE_COUNT,
           TOLERANCE_TIER1_SECONDS, TOLERANCE_TIER2_SECONDS);

    for (size_t i = 0; i < SOLAR_FIXTURE_COUNT; i++) {
        const solar_fixture_t* f = &SOLAR_FIXTURES[i];

        solar_events_t got = {0};
        int rc = solar_compute(f->lat, f->lon, f->year, f->month, f->day,
                               f->zenith_deg, &got);
        if (rc != 0) {
            printf("  [%2zu] %-44s  solar_compute returned %d  FAIL\n",
                   i, f->note, rc);
            failed += 4;
            continue;
        }

        long err_m, err_e;
        char msg_m[96], msg_e[96];
        int ok_m = compare_event("sr", got.sunrise, f->expected_morning_utc,
                                 f->tolerance_seconds, &err_m, msg_m, sizeof msg_m);
        int ok_e = compare_event("ss", got.sunset, f->expected_evening_utc,
                                 f->tolerance_seconds, &err_e, msg_e, sizeof msg_e);

        // Structural invariants — solar_noon always defined; solar_midnight
        // exactly 12 h before solar_noon by construction.
        int ok_noon = (got.solar_noon != SOLAR_NO_EVENT);
        int ok_mid  = (got.solar_midnight == got.solar_noon - 43200);

        if (ok_m) passed++; else failed++;
        if (ok_e) passed++; else failed++;
        if (ok_noon) passed++; else failed++;
        if (ok_mid)  passed++; else failed++;

        // Track worst tier-{1,2} numeric error (skips NO_EVENT cases).
        if (f->expected_morning_utc != SOLAR_NO_EVENT) {
            long abs_m = err_m < 0 ? -err_m : err_m;
            if (f->tolerance_seconds == TOLERANCE_TIER1_SECONDS) {
                if (abs_m > worst_tier1) worst_tier1 = abs_m;
            } else {
                if (abs_m > worst_tier2) worst_tier2 = abs_m;
            }
        }
        if (f->expected_evening_utc != SOLAR_NO_EVENT) {
            long abs_e = err_e < 0 ? -err_e : err_e;
            if (f->tolerance_seconds == TOLERANCE_TIER1_SECONDS) {
                if (abs_e > worst_tier1) worst_tier1 = abs_e;
            } else {
                if (abs_e > worst_tier2) worst_tier2 = abs_e;
            }
        }

        printf("  [%2zu] %-44s  %s  %s  noon=%s mid=%s\n",
               i, f->note, msg_m, msg_e,
               ok_noon ? "OK" : "FAIL",
               ok_mid  ? "OK" : "FAIL");

        if (!ok_m || !ok_e) {
            char b1[32], b2[32];
            printf("       computed sunrise %s, expected %s\n",
                   iso_utc(got.sunrise, b1),
                   iso_utc(f->expected_morning_utc, b2));
            printf("       computed sunset  %s, expected %s\n",
                   iso_utc(got.sunset,  b1),
                   iso_utc(f->expected_evening_utc,  b2));
        }
    }

    printf("\n%d passed, %d failed (worst tier-1 error %ld s,"
           " worst tier-2 error %ld s)\n",
           passed, failed, worst_tier1, worst_tier2);
    return failed == 0 ? 0 : 1;
}
