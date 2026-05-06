// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// See calendar.h for the public-API contract.
//
// Threading. Same single-recursive-mutex model as anchors.c — see the
// rationale at the top of that file. We do NOT share the anchors lock
// because the two modules can be exercised independently in tests; we
// rely on the cross-namespace collision check making sequential
// (non-atomic) lookups, and on the FastCGI handler being the only
// writer at any one time. (The test agent confirms create-collision
// rejection by issuing serial requests; concurrent operator edits from
// distinct sessions are out of scope per the contract.)

#define _GNU_SOURCE
#include "calendar.h"
#include "anchors.h"
#include "log.h"
#include "persistence.h"
#include "timers.h"
#include "acap/ACAP.h"
#include "acap/cJSON.h"

#include <ctype.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Forward decl from anchors.c — used for the cross-namespace collision
// check at create / replace_all time.
extern int anchors_id_exists(const char* id);

// Linkage for anchors.c's "is this id a calendar entry?" probe — used
// by the FR-7.1 dependency-existence check on POST /anchors. Returns 1
// if `id` matches a current calendar entry, 0 otherwise.
int calendar_id_exists(const char* id);

// Look-ahead horizon for next-occurrence searches. 365 days covers any
// annual entry; for date_range / single_date, occurrences past 365 days
// are simply reported as "no occurrence in window."
#define CALENDAR_LOOKAHEAD_DAYS 365

// ---------------------------------------------------------------------
// State
// ---------------------------------------------------------------------

static calendar_entry_t g_entries[CALENDAR_OPERATOR_MAX];
static size_t           g_count = 0;
static GMutex           g_state_lock;
static int              g_initialized = 0;

static void state_lock(void)   { g_mutex_lock(&g_state_lock); }
static void state_unlock(void) { g_mutex_unlock(&g_state_lock); }

static void trigger_recompute(void) {
    (void)timers_recompute_now(RECOMPUTE_TRIGGER_CONFIG_CHANGE);
}

// ---------------------------------------------------------------------
// Validation helpers
// ---------------------------------------------------------------------

static int is_valid_id(const char* id) {
    if (!id) return 0;
    size_t len = strlen(id);
    if (len < 1 || len > CALENDAR_ID_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

static int is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_month(int year, int month) {
    static const int dmonth[] = {31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_leap_year(year)) return 29;
    return dmonth[month - 1];
}

static int is_valid_date_components(int year, int month, int day) {
    if (year < 1900 || year > 2100) return 0;
    if (month < 1 || month > 12) return 0;
    int dim = days_in_month(year, month);
    if (day < 1 || day > dim) return 0;
    return 1;
}

// For ANNUAL entries year is ignored — validate against a non-leap year
// so Feb 29 is rejected (per the contract). But also accept (month,day)
// pairs that *can* exist in any year (1..28 for Feb, 1..30 or 31 for
// other months).
static int is_valid_annual_md(int month, int day) {
    if (month < 1 || month > 12) return 0;
    // Feb 29 is rejected per contract — operator must pick Feb 28 or
    // Mar 1.
    if (month == 2 && day == 29) return 0;
    int dim = days_in_month(2001 /* non-leap */, month);
    if (day < 1 || day > dim) return 0;
    return 1;
}

static int validate_entry(const calendar_entry_t* e) {
    if (!is_valid_id(e->id)) return CALENDAR_ERR_INVALID;
    size_t name_len = strlen(e->name);
    if (name_len < 1 || name_len > CALENDAR_NAME_MAX) return CALENDAR_ERR_INVALID;
    if (strlen(e->notes) > CALENDAR_NOTES_MAX) return CALENDAR_ERR_INVALID;

    if (e->time_mode != CALENDAR_TIME_ALL_DAY &&
        e->time_mode != CALENDAR_TIME_SPECIFIC)
        return CALENDAR_ERR_INVALID;
    if (e->time_mode == CALENDAR_TIME_SPECIFIC) {
        if (e->time_of_day_seconds < 0 || e->time_of_day_seconds > 86399)
            return CALENDAR_ERR_INVALID;
    }

    switch (e->kind) {
        case CALENDAR_KIND_SINGLE_DATE:
            if (!is_valid_date_components(e->start_date.year,
                                          e->start_date.month,
                                          e->start_date.day))
                return CALENDAR_ERR_INVALID;
            break;
        case CALENDAR_KIND_DATE_RANGE: {
            if (!is_valid_date_components(e->start_date.year,
                                          e->start_date.month,
                                          e->start_date.day))
                return CALENDAR_ERR_INVALID;
            if (!is_valid_date_components(e->end_date.year,
                                          e->end_date.month,
                                          e->end_date.day))
                return CALENDAR_ERR_INVALID;
            // end_date >= start_date
            int s = e->start_date.year * 10000 + e->start_date.month * 100 +
                    e->start_date.day;
            int en = e->end_date.year * 10000 + e->end_date.month * 100 +
                     e->end_date.day;
            if (en < s) return CALENDAR_ERR_INVALID;
            break;
        }
        case CALENDAR_KIND_ANNUAL:
            if (!is_valid_annual_md(e->start_date.month, e->start_date.day))
                return CALENDAR_ERR_INVALID;
            break;
        default:
            return CALENDAR_ERR_INVALID;
    }
    return CALENDAR_OK;
}

// ---------------------------------------------------------------------
// JSON <-> calendar_entry_t
// ---------------------------------------------------------------------

static const char* kind_to_str(calendar_kind_t k) {
    switch (k) {
        case CALENDAR_KIND_SINGLE_DATE: return "single_date";
        case CALENDAR_KIND_DATE_RANGE:  return "date_range";
        case CALENDAR_KIND_ANNUAL:      return "annual";
    }
    return "single_date";
}
static int parse_kind(const char* s, calendar_kind_t* out) {
    if (!s) return -1;
    if (strcmp(s, "single_date") == 0) { *out = CALENDAR_KIND_SINGLE_DATE; return 0; }
    if (strcmp(s, "date_range")  == 0) { *out = CALENDAR_KIND_DATE_RANGE;  return 0; }
    if (strcmp(s, "annual")      == 0) { *out = CALENDAR_KIND_ANNUAL;      return 0; }
    return -1;
}
static const char* time_mode_to_str(calendar_time_mode_t t) {
    return t == CALENDAR_TIME_ALL_DAY ? "all_day" : "specific";
}
static int parse_time_mode(const char* s, calendar_time_mode_t* out) {
    if (!s) return -1;
    if (strcmp(s, "all_day")  == 0) { *out = CALENDAR_TIME_ALL_DAY;  return 0; }
    if (strcmp(s, "specific") == 0) { *out = CALENDAR_TIME_SPECIFIC; return 0; }
    return -1;
}

static int parse_iso_date(const char* s, calendar_date_t* out) {
    if (!s) return -1;
    int y, m, d;
    if (sscanf(s, "%4d-%2d-%2d", &y, &m, &d) != 3) return -1;
    out->year = y; out->month = m; out->day = d;
    return 0;
}

static int parse_hms(const char* s, int* out_seconds) {
    if (!s) return -1;
    int h, m, sec;
    if (sscanf(s, "%2d:%2d:%2d", &h, &m, &sec) != 3) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59) return -1;
    *out_seconds = h * 3600 + m * 60 + sec;
    return 0;
}

static int copy_string_field(const cJSON* obj, const char* key,
                             char* dst, size_t dst_size) {
    cJSON* v = cJSON_GetObjectItem((cJSON*)obj, key);
    if (!cJSON_IsString(v) || !v->valuestring) return -1;
    size_t len = strlen(v->valuestring);
    if (len + 1 > dst_size) return -1;
    memcpy(dst, v->valuestring, len + 1);
    return 0;
}

static int copy_optional_string_field(const cJSON* obj, const char* key,
                                      char* dst, size_t dst_size) {
    cJSON* v = cJSON_GetObjectItem((cJSON*)obj, key);
    if (!v) { dst[0] = '\0'; return 0; }
    if (!cJSON_IsString(v) || !v->valuestring) return -1;
    size_t len = strlen(v->valuestring);
    if (len + 1 > dst_size) return -1;
    memcpy(dst, v->valuestring, len + 1);
    return 0;
}

static int entry_from_json(const cJSON* obj, calendar_entry_t* out) {
    if (!cJSON_IsObject((cJSON*)obj) || !out) return -1;
    memset(out, 0, sizeof *out);
    out->enabled = 1;

    if (copy_string_field(obj, "id",   out->id,   sizeof out->id)   != 0) return -1;
    if (copy_string_field(obj, "name", out->name, sizeof out->name) != 0) return -1;
    if (copy_optional_string_field(obj, "notes", out->notes, sizeof out->notes) != 0)
        return -1;

    cJSON* kind_v = cJSON_GetObjectItem((cJSON*)obj, "kind");
    if (!cJSON_IsString(kind_v) || parse_kind(kind_v->valuestring, &out->kind) != 0)
        return -1;
    cJSON* tm_v = cJSON_GetObjectItem((cJSON*)obj, "time_mode");
    if (!cJSON_IsString(tm_v) || parse_time_mode(tm_v->valuestring, &out->time_mode) != 0)
        return -1;

    if (out->time_mode == CALENDAR_TIME_SPECIFIC) {
        cJSON* tod_v = cJSON_GetObjectItem((cJSON*)obj, "time_of_day");
        if (!cJSON_IsString(tod_v)) return -1;
        if (parse_hms(tod_v->valuestring, &out->time_of_day_seconds) != 0) return -1;
    } else {
        out->time_of_day_seconds = 0;
    }

    cJSON* start_v = cJSON_GetObjectItem((cJSON*)obj, "start_date");
    if (!cJSON_IsString(start_v) ||
        parse_iso_date(start_v->valuestring, &out->start_date) != 0)
        return -1;

    if (out->kind == CALENDAR_KIND_DATE_RANGE) {
        cJSON* end_v = cJSON_GetObjectItem((cJSON*)obj, "end_date");
        if (!cJSON_IsString(end_v) ||
            parse_iso_date(end_v->valuestring, &out->end_date) != 0)
            return -1;
    }

    cJSON* enabled_v = cJSON_GetObjectItem((cJSON*)obj, "enabled");
    if (enabled_v) {
        if (!cJSON_IsBool(enabled_v)) return -1;
        out->enabled = cJSON_IsTrue(enabled_v) ? 1 : 0;
    }
    return 0;
}

static cJSON* entry_to_json(const calendar_entry_t* e) {
    cJSON* o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "id",   e->id);
    cJSON_AddStringToObject(o, "name", e->name);
    cJSON_AddStringToObject(o, "kind", kind_to_str(e->kind));
    cJSON_AddStringToObject(o, "time_mode", time_mode_to_str(e->time_mode));

    if (e->time_mode == CALENDAR_TIME_SPECIFIC) {
        char buf[16];
        int s = e->time_of_day_seconds;
        snprintf(buf, sizeof buf, "%02d:%02d:%02d",
                 s / 3600, (s % 3600) / 60, s % 60);
        cJSON_AddStringToObject(o, "time_of_day", buf);
    }

    char date_buf[16];
    if (e->kind == CALENDAR_KIND_ANNUAL) {
        // Year is irrelevant; emit a placeholder so the field exists.
        snprintf(date_buf, sizeof date_buf, "%04d-%02d-%02d",
                 2000, e->start_date.month, e->start_date.day);
    } else {
        snprintf(date_buf, sizeof date_buf, "%04d-%02d-%02d",
                 e->start_date.year, e->start_date.month, e->start_date.day);
    }
    cJSON_AddStringToObject(o, "start_date", date_buf);

    if (e->kind == CALENDAR_KIND_DATE_RANGE) {
        snprintf(date_buf, sizeof date_buf, "%04d-%02d-%02d",
                 e->end_date.year, e->end_date.month, e->end_date.day);
        cJSON_AddStringToObject(o, "end_date", date_buf);
    }

    cJSON_AddStringToObject(o, "notes", e->notes);
    cJSON_AddBoolToObject(o, "enabled", e->enabled ? 1 : 0);
    return o;
}

static cJSON* entries_to_json_array_locked(void) {
    cJSON* arr = cJSON_CreateArray();
    if (!arr) return NULL;
    for (size_t i = 0; i < g_count; i++) {
        cJSON* o = entry_to_json(&g_entries[i]);
        if (!o) { cJSON_Delete(arr); return NULL; }
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static int validate_calendar_file(const cJSON* parsed, void* user_data) {
    (void)user_data;
    if (!cJSON_IsArray((cJSON*)parsed)) return 0;
    int n = cJSON_GetArraySize((cJSON*)parsed);
    if (n < 0 || n > (int)CALENDAR_OPERATOR_MAX) return 0;
    calendar_entry_t tmp[CALENDAR_OPERATOR_MAX];
    for (int i = 0; i < n; i++) {
        cJSON* el = cJSON_GetArrayItem((cJSON*)parsed, i);
        if (entry_from_json(el, &tmp[i]) != 0) return 0;
        if (validate_entry(&tmp[i]) != CALENDAR_OK) return 0;
        for (int j = 0; j < i; j++) {
            if (strcmp(tmp[i].id, tmp[j].id) == 0) return 0;
        }
    }
    return 1;
}

// ---------------------------------------------------------------------
// Init / cleanup
// ---------------------------------------------------------------------

static int find_idx_locked(const char* id) {
    for (size_t i = 0; i < g_count; i++)
        if (strcmp(g_entries[i].id, id) == 0) return (int)i;
    return -1;
}

static void load_from_file_locked(void) {
    if (!ACAP_FILE_Exists("localdata/calendar.json")) {
        LOG("calendar_init: no calendar file yet (clean install)");
        return;
    }
    cJSON* arr = ACAP_FILE_Read("localdata/calendar.json");
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        LOG_ERROR("calendar_init: calendar.json malformed; quarantining");
        persistence_quarantine("localdata/calendar.json");
        return;
    }
    int n = cJSON_GetArraySize(arr);
    int loaded = 0;
    for (int i = 0; i < n && loaded < (int)CALENDAR_OPERATOR_MAX; i++) {
        cJSON* el = cJSON_GetArrayItem(arr, i);
        calendar_entry_t e;
        if (entry_from_json(el, &e) != 0 || validate_entry(&e) != CALENDAR_OK) {
            LOG_ERROR("calendar_init: entry %d malformed; quarantining file", i);
            cJSON_Delete(arr);
            persistence_quarantine("localdata/calendar.json");
            g_count = 0;
            return;
        }
        if (find_idx_locked(e.id) >= 0) {
            LOG_ERROR("calendar_init: entry %d ('%s') duplicate; quarantining",
                    i, e.id);
            cJSON_Delete(arr);
            persistence_quarantine("localdata/calendar.json");
            g_count = 0;
            return;
        }
        g_entries[g_count++] = e;
        loaded++;
    }
    cJSON_Delete(arr);
    LOG("calendar_init: loaded %d calendar entries", loaded);
}

int calendar_init(void) {
    if (g_initialized) return 0;
    g_mutex_init(&g_state_lock);
    state_lock();
    g_count = 0;
    load_from_file_locked();
    state_unlock();
    g_initialized = 1;
    return 0;
}

void calendar_cleanup(void) {
    if (!g_initialized) return;
    state_lock();
    g_count = 0;
    state_unlock();
    g_initialized = 0;
    g_mutex_clear(&g_state_lock);
}

// ---------------------------------------------------------------------
// Read accessors
// ---------------------------------------------------------------------

size_t calendar_count(void) {
    state_lock();
    size_t n = g_count;
    state_unlock();
    return n;
}

int calendar_get_by_index(size_t index, calendar_entry_t* out) {
    if (!out) return -1;
    state_lock();
    if (index >= g_count) { state_unlock(); return -1; }
    *out = g_entries[index];
    state_unlock();
    return 0;
}

int calendar_get_by_id(const char* id, calendar_entry_t* out) {
    if (!id || !out) return -1;
    state_lock();
    int idx = find_idx_locked(id);
    if (idx < 0) { state_unlock(); return -1; }
    *out = g_entries[idx];
    state_unlock();
    return 0;
}

int calendar_id_exists(const char* id) {
    if (!id) return 0;
    state_lock();
    int found = (find_idx_locked(id) >= 0);
    state_unlock();
    return found;
}

// ---------------------------------------------------------------------
// Occurrence helpers
// ---------------------------------------------------------------------

// Compute a UTC time_t for "local civil midnight on the given Gregorian
// date in the camera's local timezone." We populate a struct tm with
// is_dst = -1 so mktime determines DST automatically.
static time_t local_midnight_of(int year, int month, int day) {
    struct tm tm = {0};
    tm.tm_year = year - 1900;
    tm.tm_mon  = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 0;
    tm.tm_min  = 0;
    tm.tm_sec  = 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

// Get y/m/d in local time for a UTC instant.
static void local_ymd_of(time_t t, int* y, int* m, int* d) {
    struct tm tm;
    localtime_r(&t, &tm);
    *y = tm.tm_year + 1900;
    *m = tm.tm_mon + 1;
    *d = tm.tm_mday;
}

// Date arithmetic: "is yA/mA/dA <= yB/mB/dB?"
static int date_le(int ya, int ma, int da, int yb, int mb, int db) {
    int a = ya * 10000 + ma * 100 + da;
    int b = yb * 10000 + mb * 100 + db;
    return a <= b;
}

// "Is yA/mA/dA == yB/mB/dB?"
static int date_eq(int ya, int ma, int da, int yb, int mb, int db) {
    return ya == yb && ma == mb && da == db;
}

// Increment a y/m/d by one day.
static void date_increment(int* y, int* m, int* d) {
    int dim = days_in_month(*y, *m);
    if (*d < dim) { (*d)++; return; }
    *d = 1;
    if (*m < 12) { (*m)++; return; }
    *m = 1; (*y)++;
}

// Returns the next UTC instant on or after `after` at which the entry
// fires. Searches up to CALENDAR_LOOKAHEAD_DAYS days.
//
// `match_end_out` (optional) gets the end-of-day-stateful instant for
// ALL_DAY entries.
//
// Returns 0 with `*out = (time_t)-1` if no occurrence within the
// look-ahead window. Returns 0 with `*out` set to the fire time
// otherwise. Returns -1 on bad input.
int calendar_next_occurrence(const char* id,
                             time_t      after,
                             time_t*     out,
                             time_t*     match_end_out) {
    if (!id || !out) return -1;
    calendar_entry_t e;
    if (calendar_get_by_id(id, &e) != 0) return -1;

    int cy, cm, cd;
    local_ymd_of(after, &cy, &cm, &cd);

    for (int probe = 0; probe < CALENDAR_LOOKAHEAD_DAYS; probe++) {
        int hit = 0;
        switch (e.kind) {
            case CALENDAR_KIND_SINGLE_DATE:
                hit = date_eq(cy, cm, cd,
                              e.start_date.year, e.start_date.month, e.start_date.day);
                break;
            case CALENDAR_KIND_DATE_RANGE:
                hit = date_le(e.start_date.year, e.start_date.month, e.start_date.day,
                              cy, cm, cd) &&
                      date_le(cy, cm, cd,
                              e.end_date.year, e.end_date.month, e.end_date.day);
                break;
            case CALENDAR_KIND_ANNUAL:
                hit = (e.start_date.month == cm) && (e.start_date.day == cd);
                break;
        }
        if (hit) {
            time_t midnight = local_midnight_of(cy, cm, cd);
            if (midnight == (time_t)-1) {
                *out = (time_t)-1;
                return 0;
            }
            time_t fire = midnight;
            if (e.time_mode == CALENDAR_TIME_SPECIFIC)
                fire = midnight + (time_t)e.time_of_day_seconds;
            // Skip same-day specific fires that have already passed.
            if (e.time_mode == CALENDAR_TIME_SPECIFIC && fire < after) {
                date_increment(&cy, &cm, &cd);
                continue;
            }
            *out = fire;
            if (match_end_out) {
                if (e.time_mode == CALENDAR_TIME_ALL_DAY) {
                    // For DATE_RANGE entries, end at midnight the day
                    // after end_date; for SINGLE_DATE / ANNUAL on this
                    // day, end at next-day midnight.
                    int ey = cy, em = cm, ed = cd;
                    if (e.kind == CALENDAR_KIND_DATE_RANGE) {
                        ey = e.end_date.year;
                        em = e.end_date.month;
                        ed = e.end_date.day;
                    }
                    date_increment(&ey, &em, &ed);
                    *match_end_out = local_midnight_of(ey, em, ed);
                } else {
                    *match_end_out = (time_t)0;
                }
            }
            return 0;
        }
        date_increment(&cy, &cm, &cd);
    }

    *out = (time_t)-1;
    if (match_end_out) *match_end_out = (time_t)0;
    return 0;
}

int calendar_is_active_at(const char* id, time_t when) {
    if (!id) return -1;
    calendar_entry_t e;
    if (calendar_get_by_id(id, &e) != 0) return -1;

    int cy, cm, cd;
    local_ymd_of(when, &cy, &cm, &cd);

    int day_match = 0;
    switch (e.kind) {
        case CALENDAR_KIND_SINGLE_DATE:
            day_match = date_eq(cy, cm, cd,
                                e.start_date.year, e.start_date.month, e.start_date.day);
            break;
        case CALENDAR_KIND_DATE_RANGE:
            day_match = date_le(e.start_date.year, e.start_date.month, e.start_date.day,
                                cy, cm, cd) &&
                        date_le(cy, cm, cd,
                                e.end_date.year, e.end_date.month, e.end_date.day);
            break;
        case CALENDAR_KIND_ANNUAL:
            day_match = (e.start_date.month == cm) && (e.start_date.day == cd);
            break;
    }
    if (!day_match) return 0;

    if (e.time_mode == CALENDAR_TIME_ALL_DAY) return 1;

    // SPECIFIC: active within ±1 s of the fire instant.
    time_t midnight = local_midnight_of(cy, cm, cd);
    time_t fire = midnight + e.time_of_day_seconds;
    if (when >= fire - 1 && when <= fire + 1) return 1;
    return 0;
}

// ---------------------------------------------------------------------
// Mutators
// ---------------------------------------------------------------------

static int entry_is_stateful(const calendar_entry_t* e) {
    return e->time_mode == CALENDAR_TIME_ALL_DAY;
}

static int persist_locked(void) {
    cJSON* arr = entries_to_json_array_locked();
    if (!arr) return CALENDAR_ERR_INTERNAL;
    int rc = persistence_write_atomic("localdata/calendar.json", arr,
                                      validate_calendar_file, NULL);
    cJSON_Delete(arr);
    return (rc == 0) ? CALENDAR_OK : CALENDAR_ERR_PERSIST;
}

int calendar_create(const calendar_entry_t* in) {
    if (!in) return CALENDAR_ERR_INVALID;
    int rc = validate_entry(in);
    if (rc != CALENDAR_OK) return rc;

    state_lock();
    if (g_count >= CALENDAR_OPERATOR_MAX) {
        state_unlock();
        return CALENDAR_ERR_FULL;
    }
    if (find_idx_locked(in->id) >= 0) {
        state_unlock();
        return CALENDAR_ERR_DUPLICATE;
    }
    state_unlock();

    if (anchors_id_exists(in->id)) return CALENDAR_ERR_DUPLICATE;

    state_lock();
    size_t prev_count = g_count;
    g_entries[g_count] = *in;
    g_entries[g_count].enabled = 1;
    calendar_entry_t* slot = &g_entries[g_count];
    g_count++;

    if (persist_locked() != CALENDAR_OK) {
        g_count = prev_count;
        state_unlock();
        return CALENDAR_ERR_PERSIST;
    }

    if (ACAP_EVENTS_Add_Event(slot->id, slot->name,
                              entry_is_stateful(slot)) == 0) {
        g_count = prev_count;
        (void)persist_locked();
        state_unlock();
        return CALENDAR_ERR_REGISTER;
    }
    state_unlock();

    trigger_recompute();
    return CALENDAR_OK;
}

int calendar_update(const calendar_entry_t* in) {
    if (!in) return CALENDAR_ERR_INVALID;
    int rc = validate_entry(in);
    if (rc != CALENDAR_OK) return rc;

    state_lock();
    int idx = find_idx_locked(in->id);
    if (idx < 0) { state_unlock(); return CALENDAR_ERR_NOT_FOUND; }

    calendar_entry_t prev = g_entries[idx];
    g_entries[idx] = *in;
    g_entries[idx].enabled = prev.enabled;  // toggle store wins

    if (persist_locked() != CALENDAR_OK) {
        g_entries[idx] = prev;
        state_unlock();
        return CALENDAR_ERR_PERSIST;
    }

    ACAP_EVENTS_Remove_Event(g_entries[idx].id);
    if (ACAP_EVENTS_Add_Event(g_entries[idx].id, g_entries[idx].name,
                              entry_is_stateful(&g_entries[idx])) == 0) {
        g_entries[idx] = prev;
        (void)persist_locked();
        state_unlock();
        return CALENDAR_ERR_REGISTER;
    }
    state_unlock();

    trigger_recompute();
    return CALENDAR_OK;
}

int calendar_delete(const char* id) {
    if (!is_valid_id(id)) return CALENDAR_ERR_INVALID;

    state_lock();
    int idx = find_idx_locked(id);
    if (idx < 0) { state_unlock(); return CALENDAR_ERR_NOT_FOUND; }

    calendar_entry_t prev = g_entries[idx];
    for (size_t i = (size_t)idx; i + 1 < g_count; i++)
        g_entries[i] = g_entries[i + 1];
    g_count--;

    if (persist_locked() != CALENDAR_OK) {
        for (size_t i = g_count; i > (size_t)idx; i--)
            g_entries[i] = g_entries[i - 1];
        g_entries[idx] = prev;
        g_count++;
        state_unlock();
        return CALENDAR_ERR_PERSIST;
    }

    if (ACAP_EVENTS_Remove_Event(prev.id) != 0)
        LOG_WARN("calendar_delete: ACAP_EVENTS_Remove_Event('%s') non-zero",
                 prev.id);
    state_unlock();

    trigger_recompute();
    return CALENDAR_OK;
}

int calendar_replace_all(const calendar_entry_t* entries, size_t count) {
    if (!entries && count > 0) return CALENDAR_ERR_INVALID;
    if (count > CALENDAR_OPERATOR_MAX) return CALENDAR_ERR_FULL;

    for (size_t i = 0; i < count; i++) {
        int rc = validate_entry(&entries[i]);
        if (rc != CALENDAR_OK) return rc;
        for (size_t j = 0; j < i; j++)
            if (strcmp(entries[i].id, entries[j].id) == 0)
                return CALENDAR_ERR_DUPLICATE;
    }
    for (size_t i = 0; i < count; i++) {
        if (anchors_id_exists(entries[i].id))
            return CALENDAR_ERR_DUPLICATE;
    }

    state_lock();
    calendar_entry_t prev[CALENDAR_OPERATOR_MAX];
    size_t prev_count = g_count;
    for (size_t i = 0; i < prev_count; i++) prev[i] = g_entries[i];

    for (size_t i = 0; i < prev_count; i++)
        (void)ACAP_EVENTS_Remove_Event(prev[i].id);

    g_count = 0;
    for (size_t i = 0; i < count; i++) {
        g_entries[g_count] = entries[i];
        g_entries[g_count].enabled = 1;
        g_count++;
    }

    if (persist_locked() != CALENDAR_OK) {
        g_count = prev_count;
        for (size_t i = 0; i < prev_count; i++) {
            g_entries[i] = prev[i];
            ACAP_EVENTS_Add_Event(prev[i].id, prev[i].name,
                                  entry_is_stateful(&prev[i]));
        }
        state_unlock();
        return CALENDAR_ERR_PERSIST;
    }
    for (size_t i = 0; i < g_count; i++) {
        (void)ACAP_EVENTS_Add_Event(g_entries[i].id, g_entries[i].name,
                                    entry_is_stateful(&g_entries[i]));
    }
    state_unlock();

    trigger_recompute();
    return CALENDAR_OK;
}
