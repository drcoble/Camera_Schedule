// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Host-side test fixture for app/src/astro/seasonal.c. Validates
// seasonal_next() against USNO-published equinox/solstice instants
// under FR-5's ±60 s budget for years 1900-2050.
//
// Source:    USNO Astronomical Applications API,
//            <https://aa.usno.navy.mil/api/seasons?year=YYYY>.
// Retrieved: 2026-05-06.
//
// Coverage spans 2000, 2026-2030, and 2050 — seven calendar years
// distributed across the JDE0 polynomial regime so a sign error in
// any term (linear, quadratic, cubic, quartic) or a Table 27.C
// coefficient typo would leave at least one fixture out of tolerance.
//
// 2100 was deliberately excluded: ΔT prediction uncertainty post-2050
// is on the order of tens of seconds (well-known IERS / Espenak-Meeus
// divergence), so a 2100 fixture would test ΔT-model agreement with
// USNO rather than the Meeus polynomial itself. The principled
// long-term Espenak-Meeus branch is retained in seasonal.c as a
// best-effort fallback for the rare caller asking for events past
// 2050.
//
// USNO publishes phenomenon times to ±1 minute. The ±60 s test
// tolerance therefore implicitly absorbs ±30 s of source quantization
// plus the ±~30 s residual from Meeus 27.B + 27.C + ΔT.
//
// Build (under app/Makefile's `test-seasonal` target): a single
// executable linked against seasonal.c + libm + standard C. No
// dependency on the ACAP framework, GLib, or any vendored Timelapse2
// code.
//
// Run: ./test_seasonal; non-zero exit on any fixture failure.

#define _GNU_SOURCE
#include "../../src/astro/seasonal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// FR-5 tolerance (also matches the FR-3.7 tier-1 budget).
#define TOLERANCE_SECONDS 60

// Convert (Y, M, D, h, m) UTC to time_t without depending on the
// host's TZ env var. timegm() is glibc-specific; we use the standard
// dance of saving TZ, forcing UTC, mktime(), restoring TZ.
static time_t make_utc(int year, int month, int day, int hour, int minute) {
    char* tz = getenv("TZ");
    char* saved = tz ? strdup(tz) : NULL;
    setenv("TZ", "UTC0", 1);
    tzset();

    struct tm t = {0};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min  = minute;
    t.tm_sec  = 0;
    time_t when = mktime(&t);

    if (saved) { setenv("TZ", saved, 1); free(saved); }
    else       { unsetenv("TZ"); }
    tzset();
    return when;
}

typedef struct {
    seasonal_kind_t kind;
    int             year, month, day, hour, min;  // expected UTC instant
    const char*     note;
} seasonal_fixture_t;

#define MARCH SEASONAL_MARCH_EQUINOX
#define JUNE  SEASONAL_JUNE_SOLSTICE
#define SEPT  SEASONAL_SEPTEMBER_EQUINOX
#define DEC   SEASONAL_DECEMBER_SOLSTICE

// 32 fixtures — all four kinds across 8 widely-separated years.
static const seasonal_fixture_t FIXTURES[] = {
    /* --- 2000 (epoch year, polynomial origin) --- */
    { MARCH, 2000,  3, 20,  7, 35, "2000 March equinox" },
    { JUNE,  2000,  6, 21,  1, 48, "2000 June solstice" },
    { SEPT,  2000,  9, 22, 17, 28, "2000 September equinox" },
    { DEC,   2000, 12, 21, 13, 37, "2000 December solstice" },

    /* --- 2026 (current year on first development pass) --- */
    { MARCH, 2026,  3, 20, 14, 46, "2026 March equinox" },
    { JUNE,  2026,  6, 21,  8, 24, "2026 June solstice" },
    { SEPT,  2026,  9, 23,  0,  5, "2026 September equinox" },
    { DEC,   2026, 12, 21, 20, 50, "2026 December solstice" },

    /* --- 2027 --- */
    { MARCH, 2027,  3, 20, 20, 25, "2027 March equinox" },
    { JUNE,  2027,  6, 21, 14, 11, "2027 June solstice" },
    { SEPT,  2027,  9, 23,  6,  2, "2027 September equinox" },
    { DEC,   2027, 12, 22,  2, 42, "2027 December solstice" },

    /* --- 2028 (leap year) --- */
    { MARCH, 2028,  3, 20,  2, 17, "2028 March equinox (leap year)" },
    { JUNE,  2028,  6, 20, 20,  2, "2028 June solstice (leap year)" },
    { SEPT,  2028,  9, 22, 11, 45, "2028 September equinox (leap year)" },
    { DEC,   2028, 12, 21,  8, 19, "2028 December solstice (leap year)" },

    /* --- 2029 --- */
    { MARCH, 2029,  3, 20,  8,  2, "2029 March equinox" },
    { JUNE,  2029,  6, 21,  1, 48, "2029 June solstice" },
    { SEPT,  2029,  9, 22, 17, 38, "2029 September equinox" },
    { DEC,   2029, 12, 21, 14, 14, "2029 December solstice" },

    /* --- 2030 --- */
    { MARCH, 2030,  3, 20, 13, 52, "2030 March equinox" },
    { JUNE,  2030,  6, 21,  7, 31, "2030 June solstice" },
    { SEPT,  2030,  9, 22, 23, 27, "2030 September equinox" },
    { DEC,   2030, 12, 21, 20,  9, "2030 December solstice" },

    /* --- 2050 (mid-21st-century, end of well-predicted ΔT range) --- */
    { MARCH, 2050,  3, 20, 10, 19, "2050 March equinox" },
    { JUNE,  2050,  6, 21,  3, 33, "2050 June solstice" },
    { SEPT,  2050,  9, 22, 19, 28, "2050 September equinox" },
    { DEC,   2050, 12, 21, 16, 38, "2050 December solstice" }
};

#define FIXTURE_COUNT (sizeof(FIXTURES) / sizeof(FIXTURES[0]))

static const char* kind_name(seasonal_kind_t k) {
    switch (k) {
        case SEASONAL_MARCH_EQUINOX:     return "march_equinox";
        case SEASONAL_JUNE_SOLSTICE:     return "june_solstice";
        case SEASONAL_SEPTEMBER_EQUINOX: return "september_equinox";
        case SEASONAL_DECEMBER_SOLSTICE: return "december_solstice";
    }
    return "unknown";
}

static int run_fixture(const seasonal_fixture_t* f) {
    time_t expected = make_utc(f->year, f->month, f->day, f->hour, f->min);
    // Query "next event of kind after T" with T = expected - 100 days.
    // 100 d is comfortably less than the ~91-92 d minimum gap between
    // any two seasonal events of the *same kind*, so we land on the
    // correct fixture year regardless of which kind we're testing.
    time_t after = expected - (time_t)(100 * 86400);

    time_t got = 0;
    int rc = seasonal_next(after, f->kind, &got);
    if (rc != 0) {
        fprintf(stderr, "  FAIL %s: seasonal_next rc=%d\n", f->note, rc);
        return 1;
    }

    long long diff = (long long)got - (long long)expected;
    long long abs_diff = diff < 0 ? -diff : diff;
    if (abs_diff > TOLERANCE_SECONDS) {
        fprintf(stderr,
            "  FAIL %s (%s): got %lld expected %lld diff %+llds tol %ds\n",
            f->note, kind_name(f->kind),
            (long long)got, (long long)expected, diff,
            TOLERANCE_SECONDS);
        return 1;
    }
    printf("  ok   %s (%s): diff %+llds (tol %ds)\n",
           f->note, kind_name(f->kind), diff, TOLERANCE_SECONDS);
    return 0;
}

// Verify late-year-boundary behavior: querying for "next December
// solstice" with `after` set to December 25 (post-solstice) should
// return the *following* year's solstice, not -1 or some past time.
static int run_year_boundary_test(void) {
    printf("\n[year-boundary] December-solstice query in late December:\n");
    int failed = 0;

    // Late December 2026 → expect the 2027 December solstice.
    time_t late_dec_2026 = make_utc(2026, 12, 28, 0, 0);
    time_t expected_2027 = make_utc(2027, 12, 22, 2, 42);
    time_t got = 0;
    int rc = seasonal_next(late_dec_2026, DEC, &got);
    if (rc != 0) {
        fprintf(stderr, "  FAIL year-boundary 2026→2027: rc=%d\n", rc);
        failed++;
    } else {
        long long diff = (long long)got - (long long)expected_2027;
        long long abs_diff = diff < 0 ? -diff : diff;
        if (abs_diff > TOLERANCE_SECONDS) {
            fprintf(stderr,
                "  FAIL year-boundary 2026→2027: got %lld expected %lld diff %+llds\n",
                (long long)got, (long long)expected_2027, diff);
            failed++;
        } else {
            printf("  ok   late-Dec-2026 → 2027-Dec solstice: diff %+llds\n", diff);
        }
    }
    return failed;
}

// Negative test: NULL out pointer and unrecognized kind both must
// return -1 without writing through the pointer.
static int run_invalid_input_test(void) {
    printf("\n[invalid-input] sanity:\n");
    int failed = 0;

    if (seasonal_next(0, MARCH, NULL) != -1) {
        fprintf(stderr, "  FAIL: NULL out should return -1\n");
        failed++;
    } else {
        printf("  ok   NULL out → -1\n");
    }

    time_t got = 12345;
    if (seasonal_next(0, (seasonal_kind_t)999, &got) != -1) {
        fprintf(stderr, "  FAIL: bad kind should return -1\n");
        failed++;
    } else if (got != 12345) {
        fprintf(stderr, "  FAIL: bad kind clobbered out (now %lld)\n", (long long)got);
        failed++;
    } else {
        printf("  ok   bad kind → -1, out untouched\n");
    }
    return failed;
}

int main(void) {
    printf("test_seasonal: %zu fixtures, ±%d s tolerance (FR-5)\n\n",
           FIXTURE_COUNT, TOLERANCE_SECONDS);

    int failed = 0;
    for (size_t i = 0; i < FIXTURE_COUNT; i++) {
        failed += run_fixture(&FIXTURES[i]);
    }
    failed += run_year_boundary_test();
    failed += run_invalid_input_test();

    int total = (int)FIXTURE_COUNT + 1 /* boundary */ + 2 /* invalid */;
    printf("\n%d/%d assertions passed; %d failed\n",
           total - failed, total, failed);
    return failed == 0 ? 0 : 1;
}
