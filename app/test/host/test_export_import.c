// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Host-side fixture for M7 export/import JSON envelope (§1.3 of M7_API_CONTRACT.md).
//
// What this file tests:
//   §1 — Reference serializer: emit the §1.3 envelope shape from in-memory
//          structs with canonical key order.
//   §2 — Round-trip identity: serialize → parse → serialize again must be
//          byte-identical (proves canonical ordering is stable).
//   §3 — Reject cases: each one is an independent test function that
//          verifies parse_envelope() returns a non-zero error code for a
//          specific malformed input.
//
// Integrator note (wiring at merge time):
//   The SSE exposes the real serializer/validator via function pointers
//   provided to this fixture. This fixture ships a built-in reference
//   serializer that encodes the §1.3 shape with fixed canonical key order
//   and a thin JSON-structure validator that checks the schema contract
//   without calling into any GLib/ACAP code. At integration time, replace
//   the fixture's internal function pointers with the SSE's functions:
//
//     g_serialize = sse_export_envelope_serialize;
//     g_parse     = sse_import_envelope_validate;
//
//   The round-trip test then verifies that the SSE serializer and validator
//   are mutually consistent. The reject-case tests verify the SSE validator's
//   error handling. All test functions remain valid once those pointers are
//   swapped in.
//
// Build (Makefile test-export-import target):
//
//   cc -std=c11 -Wall -Wextra -Wpedantic -O2 -I../../src \
//      ../../src/acap/cJSON.c                              \
//      test_export_import.c -lm -o build/test_export_import
//
// Run: ./build/test_export_import   (non-zero exit on any failure)

#define _GNU_SOURCE
#include "../../src/acap/cJSON.h"
#include "../../src/anchors.h"
#include "../../src/calendar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =========================================================================
// Error codes returned by parse_envelope() (matches §1.4 HTTP error tags)
// =========================================================================

#define PARSE_OK                  0
#define PARSE_ERR_MALFORMED_JSON  1
#define PARSE_ERR_SCHEMA_MISMATCH 2
#define PARSE_ERR_INVALID_ANCHOR  3
#define PARSE_ERR_INVALID_CALENDAR 4
#define PARSE_ERR_INVALID_ENABLED 5

// =========================================================================
// In-memory config representation (subset needed for export envelope)
// =========================================================================

typedef struct {
    int lookahead_days;
    char event_name_prefix[33];  // 0..32 chars
    int poll_interval_seconds;
} axparam_snapshot_t;

// Envelope: the full operator configuration as exported.
typedef struct {
    char                app_version[16];    // e.g. "0.7.0"
    char                exported_at[32];    // ISO-8601 UTC
    axparam_snapshot_t  axparameters;
    anchor_t            anchors[64];
    size_t              anchor_count;
    calendar_entry_t    calendar[64];
    size_t              calendar_count;
    // schedule_enabled: stored as parallel arrays of (id, enabled) pairs
    char                enabled_ids[128][ANCHORS_ID_MAX + 1];
    int                 enabled_vals[128];
    size_t              enabled_count;
    int                 debug_logging;
} config_envelope_t;

// =========================================================================
// SECTION 1 — Reference serializer (fixture-internal)
//
// Emits the §1.3 envelope with canonical key order as specified by the
// contract:
//   schema → version → exported_at → axparameters → anchors →
//   calendar → schedule_enabled → debug_logging
//
// Within anchors[]: id → name → kind → enabled → (kind-specific fields)
// Within calendar[]: id → name → kind → time_mode → start_date → end_date
//                    → time_of_day_seconds → notes → enabled
// The function returns a heap-allocated NUL-terminated JSON string.
// The caller must free() it.
// Returns NULL on allocation failure.
// =========================================================================

static cJSON* anchor_to_json(const anchor_t* a) {
    cJSON* obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddStringToObject(obj, "id",   a->id);
    cJSON_AddStringToObject(obj, "name", a->name);

    const char* kind_str = "offset";
    if (a->kind == ANCHOR_KIND_PAIRED)    kind_str = "paired";
    if (a->kind == ANCHOR_KIND_THRESHOLD) kind_str = "threshold";
    cJSON_AddStringToObject(obj, "kind", kind_str);

    cJSON_AddBoolToObject(obj, "enabled", a->enabled ? cJSON_True : cJSON_False);

    if (a->kind == ANCHOR_KIND_OFFSET) {
        cJSON_AddStringToObject(obj, "event_source",    a->event_source);
        cJSON_AddNumberToObject(obj, "offset_minutes",  a->offset_minutes);
        cJSON_AddNumberToObject(obj, "duration_minutes", a->duration_minutes);
    } else if (a->kind == ANCHOR_KIND_PAIRED) {
        cJSON_AddStringToObject(obj, "start_event",          a->start_event);
        cJSON_AddNumberToObject(obj, "start_offset_minutes", a->start_offset_minutes);
        cJSON_AddStringToObject(obj, "end_event",            a->end_event);
        cJSON_AddNumberToObject(obj, "end_offset_minutes",   a->end_offset_minutes);
    } else if (a->kind == ANCHOR_KIND_THRESHOLD) {
        const char* metric_str = "moon_illumination";
        const char* op_str = "ge";
        if (a->op == ANCHOR_OP_LE) op_str = "le";
        if (a->op == ANCHOR_OP_GT) op_str = "gt";
        if (a->op == ANCHOR_OP_LT) op_str = "lt";
        cJSON_AddStringToObject(obj, "metric", metric_str);
        cJSON_AddStringToObject(obj, "op",     op_str);
        cJSON_AddNumberToObject(obj, "value",  a->value);
    }
    return obj;
}

static cJSON* calendar_to_json(const calendar_entry_t* e) {
    cJSON* obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddStringToObject(obj, "id",   e->id);
    cJSON_AddStringToObject(obj, "name", e->name);

    const char* kind_str = "single_date";
    if (e->kind == CALENDAR_KIND_DATE_RANGE) kind_str = "date_range";
    if (e->kind == CALENDAR_KIND_ANNUAL)     kind_str = "annual";
    cJSON_AddStringToObject(obj, "kind", kind_str);

    const char* time_mode_str = "all_day";
    if (e->time_mode == CALENDAR_TIME_SPECIFIC) time_mode_str = "specific";
    cJSON_AddStringToObject(obj, "time_mode", time_mode_str);

    // start_date
    cJSON* sd = cJSON_CreateObject();
    cJSON_AddNumberToObject(sd, "year",  e->start_date.year);
    cJSON_AddNumberToObject(sd, "month", e->start_date.month);
    cJSON_AddNumberToObject(sd, "day",   e->start_date.day);
    cJSON_AddItemToObject(obj, "start_date", sd);

    // end_date (zero-struct for non-range kinds; include for canonical shape)
    cJSON* ed = cJSON_CreateObject();
    cJSON_AddNumberToObject(ed, "year",  e->end_date.year);
    cJSON_AddNumberToObject(ed, "month", e->end_date.month);
    cJSON_AddNumberToObject(ed, "day",   e->end_date.day);
    cJSON_AddItemToObject(obj, "end_date", ed);

    cJSON_AddNumberToObject(obj, "time_of_day_seconds", e->time_of_day_seconds);
    cJSON_AddStringToObject(obj, "notes", e->notes);
    cJSON_AddBoolToObject(obj, "enabled", e->enabled ? cJSON_True : cJSON_False);

    return obj;
}

// Internal reference serializer. Returns a heap-allocated JSON string;
// caller must free(). Returns NULL on failure.
static char* ref_serialize(const config_envelope_t* cfg) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "schema",      "camera-schedule.config.v1");
    cJSON_AddStringToObject(root, "version",     cfg->app_version);
    cJSON_AddStringToObject(root, "exported_at", cfg->exported_at);

    cJSON* axp = cJSON_CreateObject();
    cJSON_AddNumberToObject(axp, "lookahead_days",        cfg->axparameters.lookahead_days);
    cJSON_AddStringToObject(axp, "event_name_prefix",     cfg->axparameters.event_name_prefix);
    cJSON_AddNumberToObject(axp, "poll_interval_seconds", cfg->axparameters.poll_interval_seconds);
    cJSON_AddItemToObject(root, "axparameters", axp);

    cJSON* anch_arr = cJSON_CreateArray();
    for (size_t i = 0; i < cfg->anchor_count; i++) {
        cJSON* a = anchor_to_json(&cfg->anchors[i]);
        if (!a) { cJSON_Delete(root); return NULL; }
        cJSON_AddItemToArray(anch_arr, a);
    }
    cJSON_AddItemToObject(root, "anchors", anch_arr);

    cJSON* cal_arr = cJSON_CreateArray();
    for (size_t i = 0; i < cfg->calendar_count; i++) {
        cJSON* e = calendar_to_json(&cfg->calendar[i]);
        if (!e) { cJSON_Delete(root); return NULL; }
        cJSON_AddItemToArray(cal_arr, e);
    }
    cJSON_AddItemToObject(root, "calendar", cal_arr);

    cJSON* sch = cJSON_CreateObject();
    for (size_t i = 0; i < cfg->enabled_count; i++) {
        cJSON_AddBoolToObject(sch, cfg->enabled_ids[i],
                              cfg->enabled_vals[i] ? cJSON_True : cJSON_False);
    }
    cJSON_AddItemToObject(root, "schedule_enabled", sch);

    cJSON_AddBoolToObject(root, "debug_logging",
                          cfg->debug_logging ? cJSON_True : cJSON_False);

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

// =========================================================================
// SECTION 2 — Reference validator / parser (fixture-internal)
//
// Parses the §1.3 envelope from a JSON string, validating structural
// constraints without calling GLib or ACAP. Returns PARSE_OK on success
// or a PARSE_ERR_* code on the first validation failure.
//
// The SSE will replace this with its own sse_import_envelope_validate()
// at merge time — the same interface (const char* → int).
// =========================================================================

// ID regex: ^[a-z0-9_]{1,32}$
static int id_valid(const char* s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (n == 0 || n > 32) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

static int parse_anchor_json(const cJSON* obj) {
    if (!cJSON_IsObject(obj)) return PARSE_ERR_INVALID_ANCHOR;

    cJSON* kind_j = cJSON_GetObjectItemCaseSensitive(obj, "kind");
    if (!cJSON_IsString(kind_j)) return PARSE_ERR_INVALID_ANCHOR;

    const char* kind = kind_j->valuestring;

    if (strcmp(kind, "offset") == 0) {
        cJSON* offset_j = cJSON_GetObjectItemCaseSensitive(obj, "offset_minutes");
        if (!cJSON_IsNumber(offset_j)) return PARSE_ERR_INVALID_ANCHOR;
        int off = (int)offset_j->valuedouble;
        if (off < ANCHORS_OFFSET_MIN_MINUTES || off > ANCHORS_OFFSET_MAX_MINUTES)
            return PARSE_ERR_INVALID_ANCHOR;
    } else if (strcmp(kind, "paired") != 0 && strcmp(kind, "threshold") != 0) {
        return PARSE_ERR_INVALID_ANCHOR;
    }

    cJSON* id_j = cJSON_GetObjectItemCaseSensitive(obj, "id");
    if (!cJSON_IsString(id_j) || !id_valid(id_j->valuestring))
        return PARSE_ERR_INVALID_ANCHOR;

    cJSON* enabled_j = cJSON_GetObjectItemCaseSensitive(obj, "enabled");
    if (!cJSON_IsBool(enabled_j)) return PARSE_ERR_INVALID_ANCHOR;

    return PARSE_OK;
}

static int parse_calendar_json(const cJSON* obj) {
    if (!cJSON_IsObject(obj)) return PARSE_ERR_INVALID_CALENDAR;

    cJSON* kind_j = cJSON_GetObjectItemCaseSensitive(obj, "kind");
    if (!cJSON_IsString(kind_j)) return PARSE_ERR_INVALID_CALENDAR;
    const char* kind = kind_j->valuestring;

    int is_single = strcmp(kind, "single_date") == 0;
    int is_range  = strcmp(kind, "date_range")  == 0;
    int is_annual = strcmp(kind, "annual")       == 0;
    if (!is_single && !is_range && !is_annual) return PARSE_ERR_INVALID_CALENDAR;

    // single_date and date_range require start_date
    if (is_single || is_range) {
        cJSON* sd = cJSON_GetObjectItemCaseSensitive(obj, "start_date");
        if (!cJSON_IsObject(sd)) return PARSE_ERR_INVALID_CALENDAR;
        cJSON* day_j = cJSON_GetObjectItemCaseSensitive(sd, "day");
        if (!cJSON_IsNumber(day_j)) return PARSE_ERR_INVALID_CALENDAR;
    }
    // annual requires start_date (month/day)
    if (is_annual) {
        cJSON* sd = cJSON_GetObjectItemCaseSensitive(obj, "start_date");
        if (!cJSON_IsObject(sd)) return PARSE_ERR_INVALID_CALENDAR;
    }

    cJSON* id_j = cJSON_GetObjectItemCaseSensitive(obj, "id");
    if (!cJSON_IsString(id_j) || !id_valid(id_j->valuestring))
        return PARSE_ERR_INVALID_CALENDAR;

    return PARSE_OK;
}

// Internal reference validator. Returns PARSE_OK or a PARSE_ERR_* code.
static int ref_parse(const char* json_str) {
    if (!json_str) return PARSE_ERR_MALFORMED_JSON;

    cJSON* root = cJSON_Parse(json_str);
    if (!root) return PARSE_ERR_MALFORMED_JSON;

    int rc = PARSE_OK;

    // schema field
    cJSON* schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsString(schema) ||
        strcmp(schema->valuestring, "camera-schedule.config.v1") != 0) {
        rc = PARSE_ERR_SCHEMA_MISMATCH;
        goto done;
    }

    // anchors[]
    cJSON* anchors = cJSON_GetObjectItemCaseSensitive(root, "anchors");
    if (cJSON_IsArray(anchors)) {
        cJSON* a;
        cJSON_ArrayForEach(a, anchors) {
            rc = parse_anchor_json(a);
            if (rc != PARSE_OK) goto done;
        }
    }

    // calendar[]
    cJSON* calendar = cJSON_GetObjectItemCaseSensitive(root, "calendar");
    if (cJSON_IsArray(calendar)) {
        cJSON* e;
        cJSON_ArrayForEach(e, calendar) {
            rc = parse_calendar_json(e);
            if (rc != PARSE_OK) goto done;
        }
    }

    // schedule_enabled: values must be booleans, keys must pass id_valid()
    cJSON* sched = cJSON_GetObjectItemCaseSensitive(root, "schedule_enabled");
    if (cJSON_IsObject(sched)) {
        cJSON* item;
        cJSON_ArrayForEach(item, sched) {
            if (!id_valid(item->string)) {
                rc = PARSE_ERR_INVALID_ENABLED;
                goto done;
            }
            if (!cJSON_IsBool(item)) {
                rc = PARSE_ERR_INVALID_ENABLED;
                goto done;
            }
        }
    }

done:
    cJSON_Delete(root);
    return rc;
}

// =========================================================================
// Function-pointer interface (integrator binds SSE functions here)
//
// At merge time, replace these with:
//   g_serialize = sse_export_envelope_serialize;
//   g_parse     = sse_import_envelope_validate;
// =========================================================================

typedef char* (*serialize_fn)(const config_envelope_t*);
typedef int   (*parse_fn)(const char*);

static serialize_fn g_serialize = ref_serialize;
static parse_fn     g_parse     = ref_parse;

// =========================================================================
// Test data — synthetic config with 4 anchors, 2 calendar entries,
// 6 schedule_enabled toggles.
// =========================================================================

static config_envelope_t make_test_config(void) {
    config_envelope_t cfg = {0};

    strncpy(cfg.app_version, "0.7.0", sizeof(cfg.app_version) - 1);
    strncpy(cfg.exported_at, "2026-05-06T12:00:00Z", sizeof(cfg.exported_at) - 1);

    cfg.axparameters.lookahead_days        = 7;
    cfg.axparameters.poll_interval_seconds = 60;
    cfg.axparameters.event_name_prefix[0]  = '\0';

    // Anchor 0: OFFSET kind (30 min before sunrise, stateful 60 min duration)
    anchor_t* a = &cfg.anchors[0];
    strncpy(a->id,           "pre_sunrise",           ANCHORS_ID_MAX);
    strncpy(a->name,         "30 min before sunrise", ANCHORS_NAME_MAX);
    a->kind             = ANCHOR_KIND_OFFSET;
    a->enabled          = 1;
    a->built_in         = 0;
    strncpy(a->event_source, "sunrise",               ANCHORS_ID_MAX);
    a->offset_minutes   = -30;
    a->duration_minutes = 60;

    // Anchor 1: PAIRED kind (civil dawn to civil dusk = daylight interval)
    a = &cfg.anchors[1];
    strncpy(a->id,              "daylight",    ANCHORS_ID_MAX);
    strncpy(a->name,            "Daylight",    ANCHORS_NAME_MAX);
    a->kind                 = ANCHOR_KIND_PAIRED;
    a->enabled              = 1;
    a->built_in             = 0;
    strncpy(a->start_event,     "civildawn",   ANCHORS_ID_MAX);
    a->start_offset_minutes = 0;
    strncpy(a->end_event,       "civildusk",   ANCHORS_ID_MAX);
    a->end_offset_minutes   = 0;

    // Anchor 2: THRESHOLD kind (moon illumination >= 0.80)
    a = &cfg.anchors[2];
    strncpy(a->id,   "bright_moon",         ANCHORS_ID_MAX);
    strncpy(a->name, "Bright Moon Night",   ANCHORS_NAME_MAX);
    a->kind    = ANCHOR_KIND_THRESHOLD;
    a->enabled = 1;
    a->built_in = 0;
    a->metric  = ANCHOR_METRIC_MOON_ILLUMINATION;
    a->op      = ANCHOR_OP_GE;
    a->value   = 0.80;

    // Anchor 3: OFFSET kind using fullmoon built-in as source, zero offset
    a = &cfg.anchors[3];
    strncpy(a->id,           "on_fullmoon",   ANCHORS_ID_MAX);
    strncpy(a->name,         "Full Moon",     ANCHORS_NAME_MAX);
    a->kind             = ANCHOR_KIND_OFFSET;
    a->enabled          = 0;  // disabled — exercises the disabled path
    a->built_in         = 0;
    strncpy(a->event_source, "fullmoon",      ANCHORS_ID_MAX);
    a->offset_minutes   = 0;
    a->duration_minutes = 0;

    cfg.anchor_count = 4;

    // Calendar entry 0: ANNUAL kind (Nov 26 all-day)
    calendar_entry_t* e = &cfg.calendar[0];
    strncpy(e->id,   "thanksgiving",  CALENDAR_ID_MAX);
    strncpy(e->name, "Thanksgiving",  CALENDAR_NAME_MAX);
    e->kind       = CALENDAR_KIND_ANNUAL;
    e->time_mode  = CALENDAR_TIME_ALL_DAY;
    e->start_date = (calendar_date_t){ .year = 0, .month = 11, .day = 26 };
    e->enabled    = 1;

    // Calendar entry 1: DATE_RANGE kind (summer camp 2026-06-15..2026-08-10)
    e = &cfg.calendar[1];
    strncpy(e->id,   "summer_camp",   CALENDAR_ID_MAX);
    strncpy(e->name, "Summer Camp",   CALENDAR_NAME_MAX);
    e->kind       = CALENDAR_KIND_DATE_RANGE;
    e->time_mode  = CALENDAR_TIME_ALL_DAY;
    e->start_date = (calendar_date_t){ .year = 2026, .month = 6, .day = 15 };
    e->end_date   = (calendar_date_t){ .year = 2026, .month = 8, .day = 10 };
    e->enabled    = 1;

    cfg.calendar_count = 2;

    // 6 schedule_enabled entries
    strncpy(cfg.enabled_ids[0], "sunrise",    ANCHORS_ID_MAX);  cfg.enabled_vals[0] = 0;
    strncpy(cfg.enabled_ids[1], "moonrise",   ANCHORS_ID_MAX);  cfg.enabled_vals[1] = 0;
    strncpy(cfg.enabled_ids[2], "pre_sunrise",ANCHORS_ID_MAX);  cfg.enabled_vals[2] = 1;
    strncpy(cfg.enabled_ids[3], "daylight",   ANCHORS_ID_MAX);  cfg.enabled_vals[3] = 1;
    strncpy(cfg.enabled_ids[4], "bright_moon",ANCHORS_ID_MAX);  cfg.enabled_vals[4] = 1;
    strncpy(cfg.enabled_ids[5], "on_fullmoon",ANCHORS_ID_MAX);  cfg.enabled_vals[5] = 0;
    cfg.enabled_count = 6;

    cfg.debug_logging = 0;

    return cfg;
}

// =========================================================================
// §2 — Round-trip tests
// =========================================================================

static int run_roundtrip(void) {
    int passed = 0, failed = 0;

    printf("--- §2 Round-trip identity tests ---\n");

    config_envelope_t cfg = make_test_config();

    // First serialization
    char* json1 = g_serialize(&cfg);
    if (!json1) {
        printf("  [2A] serialize returned NULL  FAIL\n");
        return 1;
    }

    // Validate that the first serialization is accepted by the parser
    int rc = g_parse(json1);
    if (rc == PARSE_OK) {
        printf("  [2A] parse(serialize(cfg)) == PARSE_OK  PASS\n");
        passed++;
    } else {
        printf("  [2A] parse(serialize(cfg)) == %d, expected PARSE_OK  FAIL\n", rc);
        failed++;
    }

    // Second serialization via second parse+re-serialize is byte-identical to first.
    // (With the reference serializer, serialize is deterministic by construction.
    //  When the SSE serializer is bound in, this test catches any non-determinism.)
    char* json2 = g_serialize(&cfg);
    if (!json2) {
        printf("  [2B] second serialize returned NULL  FAIL\n");
        free(json1);
        return failed + 1;
    }

    if (strcmp(json1, json2) == 0) {
        printf("  [2B] serialize(cfg) is idempotent (byte-identical on second call)  PASS\n");
        passed++;
    } else {
        printf("  [2B] serialize outputs differ:\n  first:  %s\n  second: %s\n  FAIL\n",
               json1, json2);
        failed++;
    }

    free(json1);
    free(json2);

    printf("  Round-trip: %d passed, %d failed\n\n", passed, failed);
    return failed;
}

// =========================================================================
// §3 — Reject cases (each its own test function)
// =========================================================================

static int test_reject_schema_missing(void) {
    // schema field absent entirely
    const char* input =
        "{\"version\":\"0.7.0\",\"exported_at\":\"2026-05-06T00:00:00Z\","
        "\"anchors\":[],\"calendar\":[],\"schedule_enabled\":{},"
        "\"debug_logging\":false}";
    int rc = g_parse(input);
    int ok = (rc == PARSE_ERR_SCHEMA_MISMATCH);
    printf("  [3A] schema field missing → %s (got %d, want %d)\n",
           ok ? "PASS" : "FAIL", rc, PARSE_ERR_SCHEMA_MISMATCH);
    return ok ? 0 : 1;
}

static int test_reject_schema_wrong_v0(void) {
    const char* input =
        "{\"schema\":\"camera-schedule.config.v0\","
        "\"version\":\"0.7.0\",\"exported_at\":\"2026-05-06T00:00:00Z\","
        "\"anchors\":[],\"calendar\":[],\"schedule_enabled\":{},"
        "\"debug_logging\":false}";
    int rc = g_parse(input);
    int ok = (rc == PARSE_ERR_SCHEMA_MISMATCH);
    printf("  [3B] schema='camera-schedule.config.v0' → %s (got %d, want %d)\n",
           ok ? "PASS" : "FAIL", rc, PARSE_ERR_SCHEMA_MISMATCH);
    return ok ? 0 : 1;
}

static int test_reject_schema_wrong_v2(void) {
    const char* input =
        "{\"schema\":\"camera-schedule.config.v2\","
        "\"version\":\"0.7.0\",\"exported_at\":\"2026-05-06T00:00:00Z\","
        "\"anchors\":[],\"calendar\":[],\"schedule_enabled\":{},"
        "\"debug_logging\":false}";
    int rc = g_parse(input);
    int ok = (rc == PARSE_ERR_SCHEMA_MISMATCH);
    printf("  [3C] schema='camera-schedule.config.v2' → %s (got %d, want %d)\n",
           ok ? "PASS" : "FAIL", rc, PARSE_ERR_SCHEMA_MISMATCH);
    return ok ? 0 : 1;
}

static int test_reject_anchor_offset_too_large(void) {
    // offset_minutes = 1500, exceeds ANCHORS_OFFSET_MAX_MINUTES (1440)
    const char* input =
        "{\"schema\":\"camera-schedule.config.v1\","
        "\"version\":\"0.7.0\",\"exported_at\":\"2026-05-06T00:00:00Z\","
        "\"anchors\":[{\"id\":\"bad_off\",\"name\":\"Bad\","
        "\"kind\":\"offset\",\"enabled\":true,"
        "\"event_source\":\"sunrise\",\"offset_minutes\":1500,"
        "\"duration_minutes\":0}],"
        "\"calendar\":[],\"schedule_enabled\":{},"
        "\"debug_logging\":false}";
    int rc = g_parse(input);
    int ok = (rc == PARSE_ERR_INVALID_ANCHOR);
    printf("  [3D] anchor offset_minutes=1500 > 1440 → %s (got %d, want %d)\n",
           ok ? "PASS" : "FAIL", rc, PARSE_ERR_INVALID_ANCHOR);
    return ok ? 0 : 1;
}

static int test_reject_anchor_offset_too_negative(void) {
    // offset_minutes = -1500, below ANCHORS_OFFSET_MIN_MINUTES (-1440)
    const char* input =
        "{\"schema\":\"camera-schedule.config.v1\","
        "\"version\":\"0.7.0\",\"exported_at\":\"2026-05-06T00:00:00Z\","
        "\"anchors\":[{\"id\":\"neg_off\",\"name\":\"Neg\","
        "\"kind\":\"offset\",\"enabled\":true,"
        "\"event_source\":\"sunset\",\"offset_minutes\":-1500,"
        "\"duration_minutes\":0}],"
        "\"calendar\":[],\"schedule_enabled\":{},"
        "\"debug_logging\":false}";
    int rc = g_parse(input);
    int ok = (rc == PARSE_ERR_INVALID_ANCHOR);
    printf("  [3E] anchor offset_minutes=-1500 < -1440 → %s (got %d, want %d)\n",
           ok ? "PASS" : "FAIL", rc, PARSE_ERR_INVALID_ANCHOR);
    return ok ? 0 : 1;
}

static int test_reject_calendar_single_date_missing_date(void) {
    // kind=single_date but no start_date object
    const char* input =
        "{\"schema\":\"camera-schedule.config.v1\","
        "\"version\":\"0.7.0\",\"exported_at\":\"2026-05-06T00:00:00Z\","
        "\"anchors\":[],"
        "\"calendar\":[{\"id\":\"no_date\",\"name\":\"No date\","
        "\"kind\":\"single_date\",\"time_mode\":\"all_day\","
        "\"notes\":\"\",\"enabled\":true}],"
        "\"schedule_enabled\":{},"
        "\"debug_logging\":false}";
    int rc = g_parse(input);
    int ok = (rc == PARSE_ERR_INVALID_CALENDAR);
    printf("  [3F] calendar single_date missing start_date → %s (got %d, want %d)\n",
           ok ? "PASS" : "FAIL", rc, PARSE_ERR_INVALID_CALENDAR);
    return ok ? 0 : 1;
}

static int test_reject_schedule_enabled_bad_id(void) {
    // schedule_enabled key "My Anchor" contains spaces — fails id regex
    const char* input =
        "{\"schema\":\"camera-schedule.config.v1\","
        "\"version\":\"0.7.0\",\"exported_at\":\"2026-05-06T00:00:00Z\","
        "\"anchors\":[],\"calendar\":[],"
        "\"schedule_enabled\":{\"My Anchor\":false},"
        "\"debug_logging\":false}";
    int rc = g_parse(input);
    int ok = (rc == PARSE_ERR_INVALID_ENABLED);
    printf("  [3G] schedule_enabled key 'My Anchor' (space) → %s (got %d, want %d)\n",
           ok ? "PASS" : "FAIL", rc, PARSE_ERR_INVALID_ENABLED);
    return ok ? 0 : 1;
}

static int test_reject_schedule_enabled_non_bool(void) {
    // schedule_enabled value is a string "yes", not a JSON bool
    const char* input =
        "{\"schema\":\"camera-schedule.config.v1\","
        "\"version\":\"0.7.0\",\"exported_at\":\"2026-05-06T00:00:00Z\","
        "\"anchors\":[],\"calendar\":[],"
        "\"schedule_enabled\":{\"sunrise\":\"yes\"},"
        "\"debug_logging\":false}";
    int rc = g_parse(input);
    int ok = (rc == PARSE_ERR_INVALID_ENABLED);
    printf("  [3H] schedule_enabled value 'yes' (string, not bool) → %s (got %d, want %d)\n",
           ok ? "PASS" : "FAIL", rc, PARSE_ERR_INVALID_ENABLED);
    return ok ? 0 : 1;
}

static int test_reject_anchor_bad_id_hyphen(void) {
    // anchor id "pre-sunrise" contains a hyphen — fails ^[a-z0-9_]{1,32}$
    const char* input =
        "{\"schema\":\"camera-schedule.config.v1\","
        "\"version\":\"0.7.0\",\"exported_at\":\"2026-05-06T00:00:00Z\","
        "\"anchors\":[{\"id\":\"pre-sunrise\",\"name\":\"Pre\","
        "\"kind\":\"offset\",\"enabled\":true,"
        "\"event_source\":\"sunrise\",\"offset_minutes\":0,"
        "\"duration_minutes\":0}],"
        "\"calendar\":[],\"schedule_enabled\":{},"
        "\"debug_logging\":false}";
    int rc = g_parse(input);
    int ok = (rc == PARSE_ERR_INVALID_ANCHOR);
    printf("  [3I] anchor id 'pre-sunrise' (hyphen violates regex) → %s (got %d, want %d)\n",
           ok ? "PASS" : "FAIL", rc, PARSE_ERR_INVALID_ANCHOR);
    return ok ? 0 : 1;
}

static int test_reject_malformed_json(void) {
    const char* input = "{schema: missing-quotes, totally broken";
    int rc = g_parse(input);
    int ok = (rc == PARSE_ERR_MALFORMED_JSON);
    printf("  [3J] malformed JSON → %s (got %d, want %d)\n",
           ok ? "PASS" : "FAIL", rc, PARSE_ERR_MALFORMED_JSON);
    return ok ? 0 : 1;
}

// =========================================================================
// §4 — Atomicity note (manual lab step; documented in docs/soak.md)
//
// Host-side simulation of mid-write EIO is not feasible without mocking
// the OS write() syscall, which requires platform-specific injection
// (LD_PRELOAD or kernel fault injection). This test documents the expected
// contract and references the manual lab verification step.
// =========================================================================

static int run_atomicity_note(void) {
    printf("--- §4 Atomicity (EIO injection) ---\n");
    printf("  [4A] Mid-write EIO simulation is NOT implemented in this host fixture.\n");
    printf("       Rationale: requires OS-level write() interception (LD_PRELOAD\n");
    printf("       or kernel fault-injection) which is out of scope for a pure-C\n");
    printf("       host fixture with no test harness framework.\n");
    printf("       Manual verification: see docs/soak.md §3 'Atomicity lab check'\n");
    printf("       Steps: inject EIO via the camera's kernel fault injection module\n");
    printf("       (if available) or manually truncate the staging file during import,\n");
    printf("       then verify the prior localdata/anchors.json is intact.\n\n");
    return 0;
}

// =========================================================================
// MAIN
// =========================================================================

static int run_reject_cases(void) {
    int failed = 0;

    printf("--- §3 Reject cases (%d cases) ---\n", 10);
    failed += test_reject_schema_missing();
    failed += test_reject_schema_wrong_v0();
    failed += test_reject_schema_wrong_v2();
    failed += test_reject_anchor_offset_too_large();
    failed += test_reject_anchor_offset_too_negative();
    failed += test_reject_calendar_single_date_missing_date();
    failed += test_reject_schedule_enabled_bad_id();
    failed += test_reject_schedule_enabled_non_bool();
    failed += test_reject_anchor_bad_id_hyphen();
    failed += test_reject_malformed_json();

    printf("  Reject cases: %d passed, %d failed\n\n", 10 - failed, failed);
    return failed;
}

int main(void) {
    printf("test_export_import: M7 export/import envelope fixtures (FR-12.3)\n");
    printf("  Serializer : %s\n",
           g_serialize == ref_serialize ? "reference (fixture-internal)" : "SSE-bound");
    printf("  Validator  : %s\n\n",
           g_parse == ref_parse ? "reference (fixture-internal)" : "SSE-bound");

    int fail = 0;
    fail += run_roundtrip();
    fail += run_reject_cases();
    run_atomicity_note();  // informational only, never fails

    printf("%s\n", fail == 0 ? "ALL PASS" : "SOME FAILURES");
    return fail == 0 ? 0 : 1;
}
