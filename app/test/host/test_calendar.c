// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Host-side test fixture for app/src/calendar.h (M6, FR-6).
//
// Two purposes:
//   (a) Syntactically exercise the public headers (anchors.h + calendar.h)
//       so any contract drift surfaces at integration compile time.
//   (b) Lock in expected-behavior assertions that integration will
//       activate once calendar.c (Phase 2) is linked.
//
// All assertions that require calendar.c carry a
//   /* TODO at integration: requires Phase 2 calendar.c */
// marker. Struct literal construction and constant assertions compile
// today against calendar.h alone; runtime assertions (calendar_create,
// calendar_is_active_at, calendar_next_occurrence) are stubs.
//
// Build (app/Makefile `test-calendar` target — Phase 2 wires the .c):
//
//   cc -std=c11 -Wall -Wextra -Wpedantic -O2 -I../../src   \
//      ../../src/calendar.c                                  \
//      test_calendar.c -lm -o build/test_calendar
//
// Run: ./build/test_calendar   (non-zero exit on any failure)

#define _GNU_SOURCE
#include "../../src/calendar.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

// =========================================================================
// UTC timestamp helper (same as test_lunar.c — host-TZ-independent)
// =========================================================================

static time_t make_utc(int year, int month, int day, int hour, int min) {
    int y = year, m = month, d = day;
    if (m <= 2) { y--; m += 12; }
    int A = y / 100;
    int B = 2 - A + A / 4;
    long jdn = (long)(365.25 * (y + 4716)) + (long)(30.6001 * (m + 1)) + d + B - 1524;
    long days = jdn - 2440588L;
    return (time_t)(days * 86400L + hour * 3600L + min * 60L);
}

// =========================================================================
// SECTION 1 — STRUCTURAL: CONSTANT AND ENUM SHAPE CHECKS
//
// These assertions compile and run without calendar.c. They verify that
// the public constants and enum values in calendar.h have not drifted from
// the values the rest of the codebase and documentation assume.
// =========================================================================

static int run_structural_checks(void) {
    int passed = 0, failed = 0;

    printf("--- §1 Structural constant and enum checks ---\n");

    // 1A. Limits
    if (CALENDAR_OPERATOR_MAX == 64) {
        printf("  [1A] CALENDAR_OPERATOR_MAX == 64  PASS\n"); passed++;
    } else {
        printf("  [1A] CALENDAR_OPERATOR_MAX == %d, expected 64  FAIL\n",
               CALENDAR_OPERATOR_MAX); failed++;
    }

    if (CALENDAR_ID_MAX == 32) {
        printf("  [1A] CALENDAR_ID_MAX == 32  PASS\n"); passed++;
    } else {
        printf("  [1A] CALENDAR_ID_MAX == %d, expected 32  FAIL\n",
               CALENDAR_ID_MAX); failed++;
    }

    if (CALENDAR_NAME_MAX == 64) {
        printf("  [1A] CALENDAR_NAME_MAX == 64  PASS\n"); passed++;
    } else {
        printf("  [1A] CALENDAR_NAME_MAX == %d, expected 64  FAIL\n",
               CALENDAR_NAME_MAX); failed++;
    }

    if (CALENDAR_NOTES_MAX == 256) {
        printf("  [1A] CALENDAR_NOTES_MAX == 256  PASS\n"); passed++;
    } else {
        printf("  [1A] CALENDAR_NOTES_MAX == %d, expected 256  FAIL\n",
               CALENDAR_NOTES_MAX); failed++;
    }

    // 1B. Kind enum values — must match JSON schema string order for
    //     clean switch/case dispatch in calendar.c.
    if (CALENDAR_KIND_SINGLE_DATE == 0) {
        printf("  [1B] CALENDAR_KIND_SINGLE_DATE == 0  PASS\n"); passed++;
    } else {
        printf("  [1B] CALENDAR_KIND_SINGLE_DATE == %d, expected 0  FAIL\n",
               (int)CALENDAR_KIND_SINGLE_DATE); failed++;
    }
    if (CALENDAR_KIND_DATE_RANGE == 1) {
        printf("  [1B] CALENDAR_KIND_DATE_RANGE == 1  PASS\n"); passed++;
    } else {
        printf("  [1B] CALENDAR_KIND_DATE_RANGE == %d, expected 1  FAIL\n",
               (int)CALENDAR_KIND_DATE_RANGE); failed++;
    }
    if (CALENDAR_KIND_ANNUAL == 2) {
        printf("  [1B] CALENDAR_KIND_ANNUAL == 2  PASS\n"); passed++;
    } else {
        printf("  [1B] CALENDAR_KIND_ANNUAL == %d, expected 2  FAIL\n",
               (int)CALENDAR_KIND_ANNUAL); failed++;
    }

    // 1C. Time mode enum values.
    if (CALENDAR_TIME_ALL_DAY == 0) {
        printf("  [1C] CALENDAR_TIME_ALL_DAY == 0  PASS\n"); passed++;
    } else {
        printf("  [1C] CALENDAR_TIME_ALL_DAY == %d, expected 0  FAIL\n",
               (int)CALENDAR_TIME_ALL_DAY); failed++;
    }
    if (CALENDAR_TIME_SPECIFIC == 1) {
        printf("  [1C] CALENDAR_TIME_SPECIFIC == 1  PASS\n"); passed++;
    } else {
        printf("  [1C] CALENDAR_TIME_SPECIFIC == %d, expected 1  FAIL\n",
               (int)CALENDAR_TIME_SPECIFIC); failed++;
    }

    // 1D. Error codes must match the anchors.h equivalents by value
    //     (calendar.h comment: "error code numbers match the anchors_*
    //      corresponding codes for the same condition").
    if (CALENDAR_OK == 0) {
        printf("  [1D] CALENDAR_OK == 0  PASS\n"); passed++;
    } else {
        printf("  [1D] CALENDAR_OK == %d, expected 0  FAIL\n",
               CALENDAR_OK); failed++;
    }
    if (CALENDAR_ERR_NOT_FOUND == -1) {
        printf("  [1D] CALENDAR_ERR_NOT_FOUND == -1  PASS\n"); passed++;
    } else {
        printf("  [1D] CALENDAR_ERR_NOT_FOUND == %d, expected -1  FAIL\n",
               CALENDAR_ERR_NOT_FOUND); failed++;
    }
    if (CALENDAR_ERR_INVALID == -3) {
        printf("  [1D] CALENDAR_ERR_INVALID == -3  PASS\n"); passed++;
    } else {
        printf("  [1D] CALENDAR_ERR_INVALID == %d, expected -3  FAIL\n",
               CALENDAR_ERR_INVALID); failed++;
    }
    if (CALENDAR_ERR_DUPLICATE == -4) {
        printf("  [1D] CALENDAR_ERR_DUPLICATE == -4  PASS\n"); passed++;
    } else {
        printf("  [1D] CALENDAR_ERR_DUPLICATE == %d, expected -4  FAIL\n",
               CALENDAR_ERR_DUPLICATE); failed++;
    }
    if (CALENDAR_ERR_FULL == -5) {
        printf("  [1D] CALENDAR_ERR_FULL == -5  PASS\n"); passed++;
    } else {
        printf("  [1D] CALENDAR_ERR_FULL == %d, expected -5  FAIL\n",
               CALENDAR_ERR_FULL); failed++;
    }
    if (CALENDAR_ERR_PERSIST == -7) {
        printf("  [1D] CALENDAR_ERR_PERSIST == -7  PASS\n"); passed++;
    } else {
        printf("  [1D] CALENDAR_ERR_PERSIST == %d, expected -7  FAIL\n",
               CALENDAR_ERR_PERSIST); failed++;
    }

    // 1E. calendar_date_t and calendar_entry_t struct are constructible
    //     (verifies field names match header). No runtime assertion needed.
    {
        calendar_date_t dt = { .year = 2026, .month = 12, .day = 25 };
        (void)dt;

        calendar_entry_t e = {0};
        strncpy(e.id,   "xmas",     CALENDAR_ID_MAX);
        strncpy(e.name, "Christmas", CALENDAR_NAME_MAX);
        e.kind             = CALENDAR_KIND_SINGLE_DATE;
        e.time_mode        = CALENDAR_TIME_ALL_DAY;
        e.time_of_day_seconds = 0;
        e.start_date       = dt;
        e.end_date         = (calendar_date_t){0};
        strncpy(e.notes, "", CALENDAR_NOTES_MAX);
        e.enabled          = 1;
        (void)e;
        printf("  [1E] calendar_entry_t struct constructible (field names OK)  PASS\n");
        passed++;
    }

    printf("\n  Structural: %d passed, %d failed\n\n", passed, failed);
    return failed;
}

// =========================================================================
// SECTION 2 — SINGLE-DATE FIXTURE
//
// FR-6 single-date entry: active exactly on the specified date, inactive
// on all other dates.
//
// Entry: { kind=SINGLE_DATE, start_date=2026-12-25, time_mode=ALL_DAY }
//
// Tests:
//   2A. On 2026-12-25T12:00:00Z — should be active (ALL_DAY: whole day).
//   2B. On 2026-12-24T23:59:59Z — should be inactive (day before).
//   2C. On 2026-12-26T00:00:01Z — should be inactive (day after).
//   2D. On 2027-12-25T12:00:00Z — should be inactive (wrong year;
//       SINGLE_DATE does not recur).
// =========================================================================

static int run_single_date_fixture(void) {
    int passed = 0, failed = 0;

    printf("--- §2 Single-date fixture (2026-12-25 ALL_DAY) ---\n");

    // Build the entry struct — syntactically exercises calendar.h.
    calendar_entry_t entry = {0};
    strncpy(entry.id,   "christmas_2026", CALENDAR_ID_MAX);
    strncpy(entry.name, "Christmas Day",  CALENDAR_NAME_MAX);
    entry.kind      = CALENDAR_KIND_SINGLE_DATE;
    entry.time_mode = CALENDAR_TIME_ALL_DAY;
    entry.start_date = (calendar_date_t){ .year = 2026, .month = 12, .day = 25 };
    entry.enabled   = 1;
    (void)entry;

    // Timestamps for the assertions.
    time_t on_day         = make_utc(2026, 12, 25, 12,  0);  // middle of the day
    time_t day_before_end = make_utc(2026, 12, 24, 23, 59);  // 1 min before midnight
    time_t day_after_start= make_utc(2026, 12, 26,  0,  1);  // 1 min into Dec 26
    time_t next_year      = make_utc(2027, 12, 25, 12,  0);  // same date, wrong year

    /* TODO at integration: requires Phase 2 calendar.c
     *
     *   calendar_init();
     *   int rc = calendar_create(&entry);
     *   ASSERT rc == CALENDAR_OK
     *
     *   // 2A: active on the day itself
     *   int active = calendar_is_active_at("christmas_2026", on_day);
     *   ASSERT active == 1
     *
     *   // 2B: inactive day before
     *   active = calendar_is_active_at("christmas_2026", day_before_end);
     *   ASSERT active == 0
     *
     *   // 2C: inactive day after
     *   active = calendar_is_active_at("christmas_2026", day_after_start);
     *   ASSERT active == 0
     *
     *   // 2D: inactive in subsequent year (SINGLE_DATE does not recur)
     *   active = calendar_is_active_at("christmas_2026", next_year);
     *   ASSERT active == 0
     *
     *   calendar_cleanup();
     */

    (void)on_day; (void)day_before_end; (void)day_after_start; (void)next_year;

    printf("  [2A] 2026-12-25T12:00:00Z on ALL_DAY entry → expected active=1  [stub]\n");
    printf("  [2B] 2026-12-24T23:59:00Z → expected active=0 (day before)  [stub]\n");
    printf("  [2C] 2026-12-26T00:00:01Z → expected active=0 (day after)  [stub]\n");
    printf("  [2D] 2027-12-25T12:00:00Z → expected active=0 (SINGLE_DATE no recur)  [stub]\n");
    passed += 4;

    printf("\n  Single-date: %d structural (TODOs at integration, %d failed)\n\n",
           passed, failed);
    return 0;  // stubs never fail until integration unlocks them
}

// =========================================================================
// SECTION 3 — DATE-RANGE FIXTURE
//
// FR-6 date-range entry: active on [start_date, end_date] inclusive.
//
// Entry: { kind=DATE_RANGE, start=2026-06-15, end=2026-08-10, ALL_DAY }
// (Mirrors the "summer_camp_2026" fixture from M6_API_CONTRACT.md §3.2.)
//
// Tests:
//   3A. Exactly on 2026-06-15 (start endpoint) → active.
//   3B. Exactly on 2026-08-10 (end endpoint) → active (inclusive).
//   3C. 2026-07-04 (middle of range) → active.
//   3D. 2026-06-14 (one day before start) → inactive.
//   3E. 2026-08-11 (one day after end) → inactive.
// =========================================================================

static int run_date_range_fixture(void) {
    printf("--- §3 Date-range fixture (2026-06-15 .. 2026-08-10 ALL_DAY) ---\n");

    calendar_entry_t entry = {0};
    strncpy(entry.id,   "summer_camp_2026", CALENDAR_ID_MAX);
    strncpy(entry.name, "Summer Camp",      CALENDAR_NAME_MAX);
    entry.kind       = CALENDAR_KIND_DATE_RANGE;
    entry.time_mode  = CALENDAR_TIME_ALL_DAY;
    entry.start_date = (calendar_date_t){ .year = 2026, .month = 6,  .day = 15 };
    entry.end_date   = (calendar_date_t){ .year = 2026, .month = 8,  .day = 10 };
    entry.enabled    = 1;
    (void)entry;

    time_t start_date  = make_utc(2026, 6, 15, 12,  0);
    time_t end_date    = make_utc(2026, 8, 10, 12,  0);
    time_t middle      = make_utc(2026, 7,  4, 12,  0);
    time_t before_start= make_utc(2026, 6, 14, 12,  0);
    time_t after_end   = make_utc(2026, 8, 11, 12,  0);
    (void)start_date; (void)end_date; (void)middle;
    (void)before_start; (void)after_end;

    /* TODO at integration: requires Phase 2 calendar.c
     *
     *   calendar_init();
     *   ASSERT calendar_create(&entry) == CALENDAR_OK
     *
     *   // 3A: start endpoint (inclusive)
     *   ASSERT calendar_is_active_at("summer_camp_2026", start_date) == 1
     *
     *   // 3B: end endpoint (inclusive)
     *   ASSERT calendar_is_active_at("summer_camp_2026", end_date) == 1
     *
     *   // 3C: midpoint
     *   ASSERT calendar_is_active_at("summer_camp_2026", middle) == 1
     *
     *   // 3D: day before start
     *   ASSERT calendar_is_active_at("summer_camp_2026", before_start) == 0
     *
     *   // 3E: day after end
     *   ASSERT calendar_is_active_at("summer_camp_2026", after_end) == 0
     *
     *   calendar_cleanup();
     */

    printf("  [3A] 2026-06-15 (start endpoint) → expected active=1  [stub]\n");
    printf("  [3B] 2026-08-10 (end endpoint, inclusive) → expected active=1  [stub]\n");
    printf("  [3C] 2026-07-04 (midpoint) → expected active=1  [stub]\n");
    printf("  [3D] 2026-06-14 (day before start) → expected active=0  [stub]\n");
    printf("  [3E] 2026-08-11 (day after end) → expected active=0  [stub]\n");

    printf("\n  Date-range: 5 structural (TODOs at integration)\n\n");
    return 0;
}

// =========================================================================
// SECTION 4 — ANNUAL FIXTURE
//
// FR-6 annual entry: recurs on the same month/day every year.
//
// Entry: { kind=ANNUAL, start_date.month=11, start_date.day=26 }
// (Mirrors "thanksgiving" from M6_API_CONTRACT.md §3.2, using a
//  fixed Nov 26 regardless of day-of-week.)
//
// Tests:
//   4A. 2026-11-26 → active.
//   4B. 2027-11-26 → active (recurs next year).
//   4C. 2028-11-26 → active (recurs again).
//   4D. 2026-11-25 (day before) → inactive.
//   4E. 2026-11-27 (day after) → inactive.
// =========================================================================

static int run_annual_fixture(void) {
    printf("--- §4 Annual fixture (month=11, day=26 ALL_DAY) ---\n");

    calendar_entry_t entry = {0};
    strncpy(entry.id,   "thanksgiving", CALENDAR_ID_MAX);
    strncpy(entry.name, "Thanksgiving", CALENDAR_NAME_MAX);
    entry.kind       = CALENDAR_KIND_ANNUAL;
    entry.time_mode  = CALENDAR_TIME_ALL_DAY;
    // For ANNUAL: only month/day are used; year is ignored by the scheduler.
    entry.start_date = (calendar_date_t){ .year = 0, .month = 11, .day = 26 };
    entry.enabled    = 1;
    (void)entry;

    time_t y2026     = make_utc(2026, 11, 26, 12,  0);
    time_t y2027     = make_utc(2027, 11, 26, 12,  0);
    time_t y2028     = make_utc(2028, 11, 26, 12,  0);
    time_t day_before= make_utc(2026, 11, 25, 12,  0);
    time_t day_after = make_utc(2026, 11, 27, 12,  0);
    (void)y2026; (void)y2027; (void)y2028;
    (void)day_before; (void)day_after;

    /* TODO at integration: requires Phase 2 calendar.c
     *
     *   calendar_init();
     *   ASSERT calendar_create(&entry) == CALENDAR_OK
     *
     *   // 4A: correct year
     *   ASSERT calendar_is_active_at("thanksgiving", y2026) == 1
     *
     *   // 4B: recurs next year
     *   ASSERT calendar_is_active_at("thanksgiving", y2027) == 1
     *
     *   // 4C: recurs again
     *   ASSERT calendar_is_active_at("thanksgiving", y2028) == 1
     *
     *   // 4D: day before
     *   ASSERT calendar_is_active_at("thanksgiving", day_before) == 0
     *
     *   // 4E: day after
     *   ASSERT calendar_is_active_at("thanksgiving", day_after) == 0
     *
     *   calendar_cleanup();
     */

    printf("  [4A] 2026-11-26 → expected active=1  [stub]\n");
    printf("  [4B] 2027-11-26 → expected active=1 (annual recurrence)  [stub]\n");
    printf("  [4C] 2028-11-26 → expected active=1 (annual recurrence)  [stub]\n");
    printf("  [4D] 2026-11-25 (day before) → expected active=0  [stub]\n");
    printf("  [4E] 2026-11-27 (day after) → expected active=0  [stub]\n");

    printf("\n  Annual: 5 structural (TODOs at integration)\n\n");
    return 0;
}

// =========================================================================
// SECTION 5 — FEB-29 REJECTION
//
// M6_API_CONTRACT.md §1.5: "Annual entries on Feb 29 are rejected at
// validation time with HTTP 400, error: 'invalid_annual_date'. We won't
// silently shift the recurrence — the operator picks Feb 28 or Mar 1."
//
// C-level: calendar_create() must return CALENDAR_ERR_INVALID for
// kind=ANNUAL, month=2, day=29.
//
// Also verify that Feb 29 in a non-annual kind is validated against the
// actual calendar date — Feb 29, 2026 does not exist (2026 is not a leap
// year), so a SINGLE_DATE entry for 2026-02-29 should also be rejected.
// =========================================================================

static int run_feb29_fixture(void) {
    printf("--- §5 Feb-29 rejection ---\n");

    // 5A. Annual entry on Feb 29 — always invalid per M6_API_CONTRACT.md.
    {
        calendar_entry_t e = {0};
        strncpy(e.id,   "feb29_annual",   CALENDAR_ID_MAX);
        strncpy(e.name, "Feb 29 Annual",  CALENDAR_NAME_MAX);
        e.kind       = CALENDAR_KIND_ANNUAL;
        e.time_mode  = CALENDAR_TIME_ALL_DAY;
        e.start_date = (calendar_date_t){ .year = 0, .month = 2, .day = 29 };
        e.enabled    = 1;
        (void)e;
        /* TODO at integration: requires Phase 2 calendar.c
         *   ASSERT calendar_create(&e) == CALENDAR_ERR_INVALID
         */
        printf("  [5A] kind=ANNUAL month=2 day=29 → expected CALENDAR_ERR_INVALID (%d)  [stub]\n",
               CALENDAR_ERR_INVALID);
    }

    // 5B. SINGLE_DATE on 2026-02-29 — 2026 is not a leap year, so this
    //     date does not exist. The validator should reject it.
    {
        calendar_entry_t e = {0};
        strncpy(e.id,   "feb29_2026",     CALENDAR_ID_MAX);
        strncpy(e.name, "Feb 29 2026",    CALENDAR_NAME_MAX);
        e.kind       = CALENDAR_KIND_SINGLE_DATE;
        e.time_mode  = CALENDAR_TIME_ALL_DAY;
        e.start_date = (calendar_date_t){ .year = 2026, .month = 2, .day = 29 };
        e.enabled    = 1;
        (void)e;
        /* TODO at integration: requires Phase 2 calendar.c
         *   ASSERT calendar_create(&e) == CALENDAR_ERR_INVALID
         *   (2026 is not a leap year; Feb 29 2026 does not exist)
         */
        printf("  [5B] kind=SINGLE_DATE 2026-02-29 (non-leap year) → expected CALENDAR_ERR_INVALID (%d)  [stub]\n",
               CALENDAR_ERR_INVALID);
    }

    // 5C. SINGLE_DATE on 2028-02-29 — 2028 IS a leap year (divisible by 4,
    //     not a century). This date is valid and should be accepted.
    {
        calendar_entry_t e = {0};
        strncpy(e.id,   "feb29_2028",     CALENDAR_ID_MAX);
        strncpy(e.name, "Feb 29 2028",    CALENDAR_NAME_MAX);
        e.kind       = CALENDAR_KIND_SINGLE_DATE;
        e.time_mode  = CALENDAR_TIME_ALL_DAY;
        e.start_date = (calendar_date_t){ .year = 2028, .month = 2, .day = 29 };
        e.enabled    = 1;
        (void)e;
        /* TODO at integration: requires Phase 2 calendar.c
         *   ASSERT calendar_create(&e) == CALENDAR_OK
         *   (2028 is a leap year; Feb 29 2028 is valid)
         */
        printf("  [5C] kind=SINGLE_DATE 2028-02-29 (leap year) → expected CALENDAR_OK (%d)  [stub]\n",
               CALENDAR_OK);
    }

    printf("\n  Feb-29 rejection: 3 structural (TODOs at integration)\n\n");
    return 0;
}

// =========================================================================
// SECTION 6 — ALL_DAY vs SPECIFIC TIME MODE
//
// CALENDAR_TIME_ALL_DAY  → stateful topic (high for the entire local
//                          civil day), fire at local 00:00.
// CALENDAR_TIME_SPECIFIC → pulse at time_of_day_seconds past local midnight.
//
// Tests:
//   6A. ALL_DAY entry: is_active_at UTC noon → 1; is_active_at next day → 0.
//   6B. SPECIFIC entry (time=20:00:00 local = 00:00:00 UTC offset unknown):
//       Use a UTC-noon anchor and verify the struct field encoding.
//       Full activation test deferred (needs timezone-aware calendar.c).
//   6C. time_of_day_seconds boundary: 0 is valid (midnight); 86399 valid
//       (last second of day); 86400 should be rejected as out of range.
// =========================================================================

static int run_all_day_vs_specific(void) {
    printf("--- §6 all_day vs specific time mode ---\n");

    // 6A. ALL_DAY shape (already exercised by §2-§4; verify time_of_day_seconds is
    //     ignored = 0 by convention).
    {
        calendar_entry_t e = {0};
        strncpy(e.id,   "all_day_entry",  CALENDAR_ID_MAX);
        strncpy(e.name, "All Day",        CALENDAR_NAME_MAX);
        e.kind                = CALENDAR_KIND_SINGLE_DATE;
        e.time_mode           = CALENDAR_TIME_ALL_DAY;
        e.time_of_day_seconds = 0;   // ignored for ALL_DAY per contract
        e.start_date          = (calendar_date_t){ .year = 2026, .month = 7, .day = 4 };
        e.enabled             = 1;
        (void)e;
        /* TODO at integration:
         *   ASSERT calendar_create(&e) == CALENDAR_OK
         *   ASSERT calendar_is_active_at("all_day_entry",
         *             make_utc(2026, 7, 4, 12, 0)) == 1   (UTC noon = day active)
         *   ASSERT calendar_is_active_at("all_day_entry",
         *             make_utc(2026, 7, 5,  0, 0)) == 0   (next UTC day)
         */
        printf("  [6A] ALL_DAY: active at UTC noon, inactive next UTC day  [stub]\n");
    }

    // 6B. SPECIFIC time mode shape: fires once at time_of_day_seconds past
    //     local midnight. Use time_of_day_seconds = 72000 (20:00:00 = 72000 s).
    {
        calendar_entry_t e = {0};
        strncpy(e.id,   "nye_party",          CALENDAR_ID_MAX);
        strncpy(e.name, "NYE Security Boost",  CALENDAR_NAME_MAX);
        e.kind                = CALENDAR_KIND_SINGLE_DATE;
        e.time_mode           = CALENDAR_TIME_SPECIFIC;
        e.time_of_day_seconds = 72000;   // 20:00:00 local (72000 s past midnight)
        e.start_date          = (calendar_date_t){ .year = 2026, .month = 12, .day = 31 };
        e.enabled             = 1;
        (void)e;
        /* TODO at integration:
         *   ASSERT calendar_create(&e) == CALENDAR_OK
         *   // calendar_next_occurrence should return an instant on 2026-12-31
         *   // at time_of_day_seconds past local midnight.
         *   time_t next = 0; time_t end = 0;
         *   ASSERT calendar_next_occurrence("nye_party",
         *             make_utc(2026, 12, 31, 0, 0), &next, &end) == 0
         *   // next should be 2026-12-31 20:00:00 local
         *   // end should be (time_t)0 (SPECIFIC = pulse, no stateful end)
         */
        printf("  [6B] SPECIFIC time_of_day_seconds=72000 (20:00:00) shape OK  [stub]\n");
    }

    // 6C. time_of_day_seconds out of range: 86400 (= 24:00:00) is invalid.
    {
        calendar_entry_t e = {0};
        strncpy(e.id,   "bad_time",       CALENDAR_ID_MAX);
        strncpy(e.name, "Bad time",       CALENDAR_NAME_MAX);
        e.kind                = CALENDAR_KIND_SINGLE_DATE;
        e.time_mode           = CALENDAR_TIME_SPECIFIC;
        e.time_of_day_seconds = 86400;   // one second past end of day
        e.start_date          = (calendar_date_t){ .year = 2026, .month = 6, .day = 1 };
        e.enabled             = 1;
        (void)e;
        /* TODO at integration:
         *   ASSERT calendar_create(&e) == CALENDAR_ERR_INVALID
         */
        printf("  [6C] time_of_day_seconds=86400 (>= 86400 is invalid)"
               " → expected CALENDAR_ERR_INVALID (%d)  [stub]\n",
               CALENDAR_ERR_INVALID);
    }

    // 6D. time_of_day_seconds boundary: 0 (midnight) and 86399 (last second) are valid.
    {
        calendar_entry_t midnight_entry = {0};
        strncpy(midnight_entry.id, "midnight_fire", CALENDAR_ID_MAX);
        midnight_entry.kind                = CALENDAR_KIND_SINGLE_DATE;
        midnight_entry.time_mode           = CALENDAR_TIME_SPECIFIC;
        midnight_entry.time_of_day_seconds = 0;       // valid: midnight
        midnight_entry.start_date          = (calendar_date_t){ .year = 2026, .month = 8, .day = 1 };
        midnight_entry.enabled             = 1;
        (void)midnight_entry;

        calendar_entry_t last_second = {0};
        strncpy(last_second.id, "last_second", CALENDAR_ID_MAX);
        last_second.kind                = CALENDAR_KIND_SINGLE_DATE;
        last_second.time_mode           = CALENDAR_TIME_SPECIFIC;
        last_second.time_of_day_seconds = 86399;    // valid: 23:59:59
        last_second.start_date          = (calendar_date_t){ .year = 2026, .month = 8, .day = 1 };
        last_second.enabled             = 1;
        (void)last_second;
        /* TODO at integration:
         *   ASSERT calendar_create(&midnight_entry) == CALENDAR_OK
         *   ASSERT calendar_create(&last_second)    == CALENDAR_OK
         */
        printf("  [6D] time_of_day_seconds=0 and 86399 are valid boundaries"
               " → both expected CALENDAR_OK (%d)  [stub]\n",
               CALENDAR_OK);
    }

    printf("\n  all_day/specific: 4 structural (TODOs at integration)\n\n");
    return 0;
}

// =========================================================================
// SECTION 7 — CALENDAR VALIDATION GATES
//
// Remaining validation rules from M6_API_CONTRACT.md §3.2 and calendar.h:
//   7A. end_date < start_date for DATE_RANGE → CALENDAR_ERR_INVALID.
//   7B. Duplicate id → CALENDAR_ERR_DUPLICATE.
//   7C. Cap exceeded (65th entry) → CALENDAR_ERR_FULL.
//   7D. ID regex mismatch ("UPPERCASE") → CALENDAR_ERR_INVALID.
// =========================================================================

static int run_validation_stubs(void) {
    printf("--- §7 Calendar validation stubs ---\n");

    // 7A. Date range where end < start.
    {
        calendar_entry_t e = {0};
        strncpy(e.id,   "inverted_range", CALENDAR_ID_MAX);
        strncpy(e.name, "Inverted",       CALENDAR_NAME_MAX);
        e.kind       = CALENDAR_KIND_DATE_RANGE;
        e.time_mode  = CALENDAR_TIME_ALL_DAY;
        e.start_date = (calendar_date_t){ .year = 2026, .month = 8, .day = 10 };
        e.end_date   = (calendar_date_t){ .year = 2026, .month = 6, .day = 15 };  // before start
        e.enabled    = 1;
        (void)e;
        /* TODO at integration:
         *   ASSERT calendar_create(&e) == CALENDAR_ERR_INVALID
         */
        printf("  [7A] DATE_RANGE end(2026-06-15) < start(2026-08-10)"
               " → expected CALENDAR_ERR_INVALID (%d)  [stub]\n",
               CALENDAR_ERR_INVALID);
    }

    // 7B. Duplicate id.
    {
        calendar_entry_t e = {0};
        strncpy(e.id,   "thanksgiving",  CALENDAR_ID_MAX);  // same id as §4
        strncpy(e.name, "Duplicate",     CALENDAR_NAME_MAX);
        e.kind       = CALENDAR_KIND_ANNUAL;
        e.time_mode  = CALENDAR_TIME_ALL_DAY;
        e.start_date = (calendar_date_t){ .year = 0, .month = 11, .day = 26 };
        e.enabled    = 1;
        (void)e;
        /* TODO at integration:
         *   // After §4's calendar_create succeeds:
         *   ASSERT calendar_create(&e) == CALENDAR_ERR_DUPLICATE
         */
        printf("  [7B] duplicate id 'thanksgiving'"
               " → expected CALENDAR_ERR_DUPLICATE (%d)  [stub]\n",
               CALENDAR_ERR_DUPLICATE);
    }

    // 7C. Operator cap exceeded.
    {
        calendar_entry_t e = {0};
        strncpy(e.id,   "entry_65",      CALENDAR_ID_MAX);
        strncpy(e.name, "65th entry",    CALENDAR_NAME_MAX);
        e.kind       = CALENDAR_KIND_ANNUAL;
        e.time_mode  = CALENDAR_TIME_ALL_DAY;
        e.start_date = (calendar_date_t){ .year = 0, .month = 1, .day = 1 };
        e.enabled    = 1;
        (void)e;
        /* TODO at integration:
         *   Create CALENDAR_OPERATOR_MAX (64) entries first.
         *   ASSERT calendar_create(&e) == CALENDAR_ERR_FULL
         */
        printf("  [7C] %d+1 calendar entries → expected CALENDAR_ERR_FULL (%d)  [stub]\n",
               CALENDAR_OPERATOR_MAX, CALENDAR_ERR_FULL);
    }

    // 7D. ID regex mismatch: uppercase is not allowed (^[a-z0-9_]{1,32}$).
    {
        calendar_entry_t e = {0};
        memcpy(e.id, "UPPERCASE", 10);
        strncpy(e.name, "Upper",  CALENDAR_NAME_MAX);
        e.kind       = CALENDAR_KIND_ANNUAL;
        e.time_mode  = CALENDAR_TIME_ALL_DAY;
        e.start_date = (calendar_date_t){ .year = 0, .month = 3, .day = 1 };
        e.enabled    = 1;
        (void)e;
        /* TODO at integration:
         *   ASSERT calendar_create(&e) == CALENDAR_ERR_INVALID
         */
        printf("  [7D] id='UPPERCASE' violates regex"
               " → expected CALENDAR_ERR_INVALID (%d)  [stub]\n",
               CALENDAR_ERR_INVALID);
    }

    printf("\n  Validation stubs: 4 structural (TODOs at integration)\n\n");
    return 0;
}

// =========================================================================
// MAIN
// =========================================================================

int main(void) {
    printf("test_calendar: M6 calendar-surface fixtures (FR-6)\n");
    printf("  §1 structural checks (constants, enums, struct shape)\n");
    printf("  §2 single-date fixture (2026-12-25 ALL_DAY)\n");
    printf("  §3 date-range fixture (2026-06-15..2026-08-10)\n");
    printf("  §4 annual fixture (month=11 day=26)\n");
    printf("  §5 Feb-29 rejection (annual always invalid; non-leap invalid)\n");
    printf("  §6 all_day vs specific time mode\n");
    printf("  §7 validation gates (inverted range, dup, cap, regex)\n\n");

    int fail = 0;
    fail += run_structural_checks();
    fail += run_single_date_fixture();
    fail += run_date_range_fixture();
    fail += run_annual_fixture();
    fail += run_feb29_fixture();
    fail += run_all_day_vs_specific();
    fail += run_validation_stubs();

    printf("%s\n", fail == 0 ? "ALL PASS" : "SOME FAILURES");
    return fail == 0 ? 0 : 1;
}
