// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// See anchors.h for the public-API contract.
//
// Threading. HTTP handlers run on a separate FastCGI pthread (see
// ACAP.c — fastcgi_thread_func runs on its own thread); GLib timer
// callbacks (and timers_recompute_now) run on the main loop thread.
// Both touch the in-memory anchor list and the schedule_enabled cache.
// We guard them with a single non-recursive GMutex `g_state_lock`.
// The "single mutex" choice is deliberate: contention is negligible
// (a handful of operator edits per day vs. tens of timer arms) and
// the simpler bookkeeping outweighs any throughput win from finer-
// grained locking. Mutators that need to consult calendar.c release
// our lock first, look up via calendar_get_by_id (which takes its own
// independent lock), then re-acquire ours — keeping the lock graph
// strictly anchors-then-calendar to avoid deadlock.
//
// Module layout:
//   1. State + lock primitives
//   2. Validation helpers (regex, range checks, kind-specific shape)
//   3. JSON <-> anchor_t serialization
//   4. Schedule-enabled store load/save
//   5. Init / cleanup / built-in seeding
//   6. Read accessors
//   7. anchors_resolve_source
//   8. CRUD mutators (create / update / delete / set_enabled / replace_all)

#define _GNU_SOURCE
#include "anchors.h"
#include "calendar.h"
#include "persistence.h"
#include "timers.h"
#include "astro/solar.h"
#include "astro/lunar.h"
#include "astro/seasonal.h"
#include "acap/ACAP.h"
#include "acap/cJSON.h"

#include <ctype.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define LOG(fmt, args...)      do { syslog(LOG_INFO,    fmt, ## args); } while (0)
#define LOG_WARN(fmt, args...) do { syslog(LOG_WARNING, fmt, ## args); } while (0)
#define LOG_ERROR(fmt, args...)  do { syslog(LOG_ERR,     fmt, ## args); } while (0)

// ---------------------------------------------------------------------
// 1. State
// ---------------------------------------------------------------------

// Sized for the worst case: 22 built-ins + ANCHORS_OPERATOR_MAX (64) =
// 86 entries. We keep a single contiguous array because the list is
// small and stable; insertions are tail-only (or via replace_all which
// rebuilds the operator slice). Built-ins occupy [0, n_built_in);
// operator anchors occupy [n_built_in, count).
#define ANCHORS_TOTAL_MAX (22 + ANCHORS_OPERATOR_MAX)

static anchor_t  g_anchors[ANCHORS_TOTAL_MAX];
static size_t    g_count       = 0;
static size_t    g_n_built_in  = 0;

// FR-11.7 enable-state cache. Mirrors localdata/schedule_enabled.json.
// Absent keys mean enabled. Stored as a flat cJSON object so we can
// round-trip directly through ACAP_FILE_Read / persistence_write_atomic.
static cJSON*    g_enabled_store = NULL;

static GMutex    g_state_lock;
static int       g_initialized = 0;

static void state_lock(void)   { g_mutex_lock(&g_state_lock); }
static void state_unlock(void) { g_mutex_unlock(&g_state_lock); }

// Linkage for calendar.c's "is this id taken?" probe — calendar.c needs
// to consult the anchor namespace at create/replace time.
int anchors_id_exists(const char* id);  // forward decl, public to .c only

// Mirror from calendar.c — used by the FR-7.1 dependency-existence
// check on POST /anchors (validates that an `event_source` reference
// resolves to something).
extern int calendar_id_exists(const char* id);

// Trigger-recompute hook: anchors mutators call this on success. timers.c
// ignores anchor slots not yet implemented; once the anchor scheduler
// pattern lands in timers.c this becomes load-bearing.
static void trigger_recompute(void) {
    (void)timers_recompute_now();
}

// ---------------------------------------------------------------------
// 2. Validation helpers
// ---------------------------------------------------------------------

// Regex equivalent: ^[a-z0-9_]{1,32}$
static int is_valid_id(const char* id) {
    if (!id) return 0;
    size_t len = strlen(id);
    if (len < 1 || len > ANCHORS_ID_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

static int is_valid_name(const char* name) {
    if (!name) return 0;
    size_t len = strlen(name);
    return len >= 1 && len <= ANCHORS_NAME_MAX;
}

// True iff `id` is currently registered as an anchor (built-in or
// operator). Caller must hold g_state_lock.
static int anchor_id_taken_locked(const char* id) {
    for (size_t i = 0; i < g_count; i++)
        if (strcmp(g_anchors[i].id, id) == 0) return 1;
    return 0;
}

int anchors_id_exists(const char* id) {
    if (!id) return 0;
    state_lock();
    int r = anchor_id_taken_locked(id);
    state_unlock();
    return r;
}

// Returns 1 iff `id` resolves in the global namespace (built-in
// anchor, operator anchor, or calendar entry). Used as the
// dependency-existence gate at create/update time per the contract
// §1.2 (HTTP 422 on missing dependency).
static int id_resolves(const char* id) {
    if (!id) return 0;
    if (anchors_id_exists(id)) return 1;
    if (calendar_id_exists(id)) return 1;
    return 0;
}

// Per-kind dependency-existence validation. Returns ANCHORS_OK or
// ANCHORS_ERR_DEP. Threshold anchors have no source dependency and
// always pass.
static int validate_anchor_dependencies(const anchor_t* a) {
    switch (a->kind) {
        case ANCHOR_KIND_OFFSET:
            if (!id_resolves(a->event_source)) return ANCHORS_ERR_DEP;
            break;
        case ANCHOR_KIND_PAIRED:
            if (!id_resolves(a->start_event)) return ANCHORS_ERR_DEP;
            if (!id_resolves(a->end_event))   return ANCHORS_ERR_DEP;
            break;
        case ANCHOR_KIND_THRESHOLD:
            break;
    }
    return ANCHORS_OK;
}

// Per-kind field validation. Returns ANCHORS_OK or an ANCHORS_ERR_*.
static int validate_anchor_fields(const anchor_t* a) {
    if (!is_valid_id(a->id))   return ANCHORS_ERR_INVALID;
    if (!is_valid_name(a->name)) return ANCHORS_ERR_INVALID;

    switch (a->kind) {
        case ANCHOR_KIND_OFFSET:
            if (!is_valid_id(a->event_source)) return ANCHORS_ERR_INVALID;
            if (a->offset_minutes < ANCHORS_OFFSET_MIN_MINUTES ||
                a->offset_minutes > ANCHORS_OFFSET_MAX_MINUTES)
                return ANCHORS_ERR_INVALID;
            if (a->duration_minutes < 0 ||
                a->duration_minutes > ANCHORS_DURATION_MAX_MINUTES)
                return ANCHORS_ERR_INVALID;
            break;

        case ANCHOR_KIND_PAIRED:
            if (!is_valid_id(a->start_event)) return ANCHORS_ERR_INVALID;
            if (!is_valid_id(a->end_event))   return ANCHORS_ERR_INVALID;
            if (a->start_offset_minutes < ANCHORS_OFFSET_MIN_MINUTES ||
                a->start_offset_minutes > ANCHORS_OFFSET_MAX_MINUTES)
                return ANCHORS_ERR_INVALID;
            if (a->end_offset_minutes < ANCHORS_OFFSET_MIN_MINUTES ||
                a->end_offset_minutes > ANCHORS_OFFSET_MAX_MINUTES)
                return ANCHORS_ERR_INVALID;
            break;

        case ANCHOR_KIND_THRESHOLD:
            // v1: only moon_illumination, value in [0, 1].
            if (a->metric != ANCHOR_METRIC_MOON_ILLUMINATION)
                return ANCHORS_ERR_INVALID;
            if (a->op != ANCHOR_OP_GE && a->op != ANCHOR_OP_LE &&
                a->op != ANCHOR_OP_GT && a->op != ANCHOR_OP_LT)
                return ANCHORS_ERR_INVALID;
            if (!(a->value >= 0.0 && a->value <= 1.0))
                return ANCHORS_ERR_INVALID;
            break;

        default:
            return ANCHORS_ERR_INVALID;
    }
    return ANCHORS_OK;
}

// ---------------------------------------------------------------------
// 3. JSON <-> anchor_t
// ---------------------------------------------------------------------

static const char* kind_to_str(anchor_kind_t k) {
    switch (k) {
        case ANCHOR_KIND_OFFSET:    return "offset";
        case ANCHOR_KIND_PAIRED:    return "paired";
        case ANCHOR_KIND_THRESHOLD: return "threshold";
    }
    return "offset";
}

static int parse_kind(const char* s, anchor_kind_t* out) {
    if (!s) return -1;
    if (strcmp(s, "offset")    == 0) { *out = ANCHOR_KIND_OFFSET;    return 0; }
    if (strcmp(s, "paired")    == 0) { *out = ANCHOR_KIND_PAIRED;    return 0; }
    if (strcmp(s, "threshold") == 0) { *out = ANCHOR_KIND_THRESHOLD; return 0; }
    return -1;
}

static const char* op_to_str(anchor_op_t o) {
    switch (o) {
        case ANCHOR_OP_GE: return "ge";
        case ANCHOR_OP_LE: return "le";
        case ANCHOR_OP_GT: return "gt";
        case ANCHOR_OP_LT: return "lt";
    }
    return "ge";
}

static int parse_op(const char* s, anchor_op_t* out) {
    if (!s) return -1;
    if (strcmp(s, "ge") == 0) { *out = ANCHOR_OP_GE; return 0; }
    if (strcmp(s, "le") == 0) { *out = ANCHOR_OP_LE; return 0; }
    if (strcmp(s, "gt") == 0) { *out = ANCHOR_OP_GT; return 0; }
    if (strcmp(s, "lt") == 0) { *out = ANCHOR_OP_LT; return 0; }
    return -1;
}

// Copy an inline char array out of a cJSON string. Returns 0 on
// success, -1 if the field is missing, not a string, or too long.
static int copy_string_field(const cJSON* obj, const char* key,
                             char* dst, size_t dst_size) {
    cJSON* v = cJSON_GetObjectItem((cJSON*)obj, key);
    if (!cJSON_IsString(v) || !v->valuestring) return -1;
    size_t len = strlen(v->valuestring);
    if (len + 1 > dst_size) return -1;
    memcpy(dst, v->valuestring, len + 1);
    return 0;
}

// Parse a JSON anchor object (operator anchor wire shape, NOT the
// built-in seed shape) into an anchor_t. `out->built_in` is forced to
// 0. Returns 0 on success, -1 on malformed JSON.
static int anchor_from_json(const cJSON* obj, anchor_t* out) {
    if (!cJSON_IsObject((cJSON*)obj) || !out) return -1;
    memset(out, 0, sizeof *out);
    out->built_in = 0;
    out->enabled  = 1;  // default; toggle store applied at init time

    if (copy_string_field(obj, "id",   out->id,   sizeof out->id)   != 0) return -1;
    if (copy_string_field(obj, "name", out->name, sizeof out->name) != 0) return -1;

    cJSON* kind_v = cJSON_GetObjectItem((cJSON*)obj, "kind");
    if (!cJSON_IsString(kind_v) || parse_kind(kind_v->valuestring, &out->kind) != 0)
        return -1;

    // `enabled` is informational on POST per the contract; new entries
    // default to enabled. We still accept it (matches the wire shape)
    // but the toggle store wins at init.
    cJSON* enabled_v = cJSON_GetObjectItem((cJSON*)obj, "enabled");
    if (enabled_v) {
        if (!cJSON_IsBool(enabled_v)) return -1;
        out->enabled = cJSON_IsTrue(enabled_v) ? 1 : 0;
    }

    switch (out->kind) {
        case ANCHOR_KIND_OFFSET: {
            if (copy_string_field(obj, "event_source",
                                  out->event_source, sizeof out->event_source) != 0)
                return -1;
            cJSON* off  = cJSON_GetObjectItem((cJSON*)obj, "offset_minutes");
            cJSON* dur  = cJSON_GetObjectItem((cJSON*)obj, "duration_minutes");
            if (!cJSON_IsNumber(off) || !cJSON_IsNumber(dur)) return -1;
            out->offset_minutes   = (int)off->valuedouble;
            out->duration_minutes = (int)dur->valuedouble;
            break;
        }
        case ANCHOR_KIND_PAIRED: {
            if (copy_string_field(obj, "start_event",
                                  out->start_event, sizeof out->start_event) != 0)
                return -1;
            if (copy_string_field(obj, "end_event",
                                  out->end_event, sizeof out->end_event) != 0)
                return -1;
            cJSON* sof = cJSON_GetObjectItem((cJSON*)obj, "start_offset_minutes");
            cJSON* eof = cJSON_GetObjectItem((cJSON*)obj, "end_offset_minutes");
            if (!cJSON_IsNumber(sof) || !cJSON_IsNumber(eof)) return -1;
            out->start_offset_minutes = (int)sof->valuedouble;
            out->end_offset_minutes   = (int)eof->valuedouble;
            break;
        }
        case ANCHOR_KIND_THRESHOLD: {
            cJSON* metric_v = cJSON_GetObjectItem((cJSON*)obj, "metric");
            cJSON* op_v     = cJSON_GetObjectItem((cJSON*)obj, "op");
            cJSON* val_v    = cJSON_GetObjectItem((cJSON*)obj, "value");
            if (!cJSON_IsString(metric_v) || !cJSON_IsString(op_v) ||
                !cJSON_IsNumber(val_v))
                return -1;
            if (strcmp(metric_v->valuestring, "moon_illumination") != 0)
                return -1;
            out->metric = ANCHOR_METRIC_MOON_ILLUMINATION;
            if (parse_op(op_v->valuestring, &out->op) != 0) return -1;
            out->value = val_v->valuedouble;
            break;
        }
    }
    return 0;
}

// Serialize one anchor into a fresh cJSON object. Used by both the GET
// /anchors response and the localdata file. Caller takes ownership of
// the returned object.
static cJSON* anchor_to_json(const anchor_t* a, int include_built_in_field) {
    cJSON* o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "id",   a->id);
    cJSON_AddStringToObject(o, "name", a->name);
    cJSON_AddStringToObject(o, "kind", kind_to_str(a->kind));
    cJSON_AddBoolToObject(o, "enabled", a->enabled ? 1 : 0);
    if (include_built_in_field)
        cJSON_AddBoolToObject(o, "built_in", a->built_in ? 1 : 0);

    switch (a->kind) {
        case ANCHOR_KIND_OFFSET:
            cJSON_AddStringToObject(o, "event_source",     a->event_source);
            cJSON_AddNumberToObject(o, "offset_minutes",   a->offset_minutes);
            cJSON_AddNumberToObject(o, "duration_minutes", a->duration_minutes);
            break;
        case ANCHOR_KIND_PAIRED:
            cJSON_AddStringToObject(o, "start_event",          a->start_event);
            cJSON_AddNumberToObject(o, "start_offset_minutes", a->start_offset_minutes);
            cJSON_AddStringToObject(o, "end_event",            a->end_event);
            cJSON_AddNumberToObject(o, "end_offset_minutes",   a->end_offset_minutes);
            break;
        case ANCHOR_KIND_THRESHOLD:
            cJSON_AddStringToObject(o, "metric", "moon_illumination");
            cJSON_AddStringToObject(o, "op",     op_to_str(a->op));
            cJSON_AddNumberToObject(o, "value",  a->value);
            break;
    }
    return o;
}

// Build a fresh cJSON array containing only the operator-defined
// anchors, suitable for atomic write to localdata/anchors.json.
static cJSON* operator_anchors_to_json_array(void) {
    cJSON* arr = cJSON_CreateArray();
    if (!arr) return NULL;
    for (size_t i = g_n_built_in; i < g_count; i++) {
        cJSON* o = anchor_to_json(&g_anchors[i], 0);  // no built_in field on disk
        if (!o) { cJSON_Delete(arr); return NULL; }
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

// Validator callback for persistence_write_atomic on anchors.json.
// Re-runs validation on the parsed-back file to satisfy FR-12.1 schema-
// validate-before-rename. Enforces:
//   * top-level array, length ≤ ANCHORS_OPERATOR_MAX
//   * every entry passes per-kind shape + range validation
//   * IDs are unique within the array (intra-batch dedupe)
//   * no entry's ID collides with a built-in anchor
static int validate_anchors_file(const cJSON* parsed, void* user_data) {
    (void)user_data;
    if (!cJSON_IsArray((cJSON*)parsed)) return 0;
    int count = cJSON_GetArraySize((cJSON*)parsed);
    if (count < 0 || count > (int)ANCHORS_OPERATOR_MAX) return 0;

    anchor_t tmp[ANCHORS_OPERATOR_MAX];
    for (int i = 0; i < count; i++) {
        cJSON* el = cJSON_GetArrayItem((cJSON*)parsed, i);
        if (anchor_from_json(el, &tmp[i]) != 0) return 0;
        if (validate_anchor_fields(&tmp[i]) != ANCHORS_OK) return 0;
        for (int j = 0; j < i; j++) {
            if (strcmp(tmp[i].id, tmp[j].id) == 0) return 0;
        }
        // Reject collision with any built-in id (built-ins are never
        // persisted to localdata/anchors.json; if one ever appeared it
        // would shadow the built-in entry in our merged in-memory list).
        for (size_t k = 0; k < g_n_built_in; k++) {
            if (strcmp(tmp[i].id, g_anchors[k].id) == 0) return 0;
        }
    }
    return 1;
}

// ---------------------------------------------------------------------
// 4. Schedule-enabled store
// ---------------------------------------------------------------------

// Returns 1 if `id` is enabled per the store. Absent keys default to
// enabled (DL-18 / FR-11.7). Caller must hold g_state_lock.
static int enabled_lookup_locked(const char* id) {
    if (!g_enabled_store || !id) return 1;
    cJSON* v = cJSON_GetObjectItem(g_enabled_store, id);
    if (!v) return 1;
    if (!cJSON_IsBool(v)) return 1;  // tolerate a malformed value
    return cJSON_IsTrue(v) ? 1 : 0;
}

// Public lock-taking variant — used by timers.c on the firing path.
int anchors_is_enabled(const char* id) {
    if (!id) return 1;
    state_lock();
    int r = enabled_lookup_locked(id);
    state_unlock();
    return r;
}

// Validator for the enable-state file: top-level object with boolean
// values. Orphan keys are tolerated per the contract.
static int validate_enabled_file(const cJSON* parsed, void* user_data) {
    (void)user_data;
    if (!cJSON_IsObject((cJSON*)parsed)) return 0;
    cJSON* child = parsed->child;
    while (child) {
        if (!cJSON_IsBool(child)) return 0;
        child = child->next;
    }
    return 1;
}

// Persist the in-memory enable-state cache atomically. Caller must hold
// g_state_lock.
static int save_enabled_store_locked(void) {
    if (!g_enabled_store) return -1;
    return persistence_write_atomic("localdata/schedule_enabled.json",
                                    g_enabled_store,
                                    validate_enabled_file, NULL);
}

// Apply the loaded enable-store to every anchor's `enabled` flag.
// Caller must hold g_state_lock.
static void apply_enabled_to_anchors_locked(void) {
    for (size_t i = 0; i < g_count; i++)
        g_anchors[i].enabled = enabled_lookup_locked(g_anchors[i].id);
}

// ---------------------------------------------------------------------
// 5. Init / built-in seeding
// ---------------------------------------------------------------------

static int seed_built_ins(void) {
    cJSON* events = ACAP_FILE_Read("settings/events.json");
    if (!events || !cJSON_IsArray(events)) {
        LOG_ERROR("anchors_init: settings/events.json missing or malformed");
        if (events) cJSON_Delete(events);
        return -1;
    }
    int n = cJSON_GetArraySize(events);
    if (n < 0) { cJSON_Delete(events); return -1; }
    if ((size_t)n > ANCHORS_TOTAL_MAX) {
        LOG_WARN("anchors_init: events.json has %d entries, capping at %d",
                 n, (int)ANCHORS_TOTAL_MAX);
        n = (int)ANCHORS_TOTAL_MAX;
    }

    for (int i = 0; i < n; i++) {
        cJSON* el = cJSON_GetArrayItem(events, i);
        cJSON* id_v   = cJSON_GetObjectItem(el, "id");
        cJSON* name_v = cJSON_GetObjectItem(el, "name");
        if (!cJSON_IsString(id_v) || !cJSON_IsString(name_v)) continue;
        if (!is_valid_id(id_v->valuestring)) {
            LOG_WARN("anchors_init: skipping invalid built-in id '%s'",
                     id_v->valuestring ? id_v->valuestring : "(null)");
            continue;
        }
        anchor_t* a = &g_anchors[g_count];
        memset(a, 0, sizeof *a);
        snprintf(a->id,   sizeof a->id,   "%s", id_v->valuestring);
        snprintf(a->name, sizeof a->name, "%s", name_v->valuestring);
        a->kind     = ANCHOR_KIND_OFFSET;
        a->built_in = 1;
        a->enabled  = 1;
        // For built-ins, event_source equals id — anchors_resolve_source
        // recognizes built-in ids and dispatches to the astro/* layer.
        snprintf(a->event_source, sizeof a->event_source, "%s",
                 id_v->valuestring);
        a->offset_minutes   = 0;
        a->duration_minutes = 0;
        g_count++;
    }
    g_n_built_in = g_count;
    cJSON_Delete(events);
    return 0;
}

// Load operator anchors from localdata/anchors.json; on parse failure,
// quarantine the file and start with no operator entries (FR-12.4).
// Caller must hold g_state_lock.
static void load_operator_anchors_locked(void) {
    if (!ACAP_FILE_Exists("localdata/anchors.json")) {
        LOG("anchors_init: no operator anchors file yet (clean install)");
        return;
    }
    cJSON* arr = ACAP_FILE_Read("localdata/anchors.json");
    if (!arr) {
        LOG_ERROR("anchors_init: localdata/anchors.json unreadable");
        persistence_quarantine("localdata/anchors.json");
        return;
    }
    if (!cJSON_IsArray(arr)) {
        LOG_ERROR("anchors_init: anchors.json is not a JSON array");
        cJSON_Delete(arr);
        persistence_quarantine("localdata/anchors.json");
        return;
    }
    int n = cJSON_GetArraySize(arr);
    int loaded = 0;
    for (int i = 0; i < n && loaded < (int)ANCHORS_OPERATOR_MAX; i++) {
        cJSON* el = cJSON_GetArrayItem(arr, i);
        anchor_t a;
        if (anchor_from_json(el, &a) != 0) {
            LOG_ERROR("anchors_init: anchors.json entry %d malformed", i);
            cJSON_Delete(arr);
            persistence_quarantine("localdata/anchors.json");
            // Roll back partial loads — operator slice starts empty.
            g_count = g_n_built_in;
            return;
        }
        if (validate_anchor_fields(&a) != ANCHORS_OK) {
            LOG_ERROR("anchors_init: anchors.json entry %d failed validation", i);
            cJSON_Delete(arr);
            persistence_quarantine("localdata/anchors.json");
            g_count = g_n_built_in;
            return;
        }
        if (anchor_id_taken_locked(a.id)) {
            LOG_ERROR("anchors_init: anchors.json entry %d ('%s') collides with built-in",
                    i, a.id);
            cJSON_Delete(arr);
            persistence_quarantine("localdata/anchors.json");
            g_count = g_n_built_in;
            return;
        }
        g_anchors[g_count] = a;
        g_count++;
        loaded++;
    }
    cJSON_Delete(arr);
    LOG("anchors_init: loaded %d operator anchors", loaded);
}

// Load the schedule_enabled.json cache. Missing/malformed file ⇒ start
// with an empty object (everything enabled). Caller must hold lock.
static void load_enabled_store_locked(void) {
    if (g_enabled_store) {
        cJSON_Delete(g_enabled_store);
        g_enabled_store = NULL;
    }
    if (!ACAP_FILE_Exists("localdata/schedule_enabled.json")) {
        g_enabled_store = cJSON_CreateObject();
        return;
    }
    cJSON* o = ACAP_FILE_Read("localdata/schedule_enabled.json");
    if (!o || !cJSON_IsObject(o)) {
        if (o) cJSON_Delete(o);
        LOG_ERROR("anchors_init: schedule_enabled.json malformed; starting empty");
        persistence_quarantine("localdata/schedule_enabled.json");
        g_enabled_store = cJSON_CreateObject();
        return;
    }
    g_enabled_store = o;
}

int anchors_init(void) {
    if (g_initialized) return 0;
    g_mutex_init(&g_state_lock);
    state_lock();

    g_count      = 0;
    g_n_built_in = 0;

    if (seed_built_ins() != 0) {
        state_unlock();
        return -1;
    }
    load_operator_anchors_locked();
    load_enabled_store_locked();
    apply_enabled_to_anchors_locked();

    g_initialized = 1;
    state_unlock();
    LOG("anchors_init: %zu total anchors (%zu built-in, %zu operator)",
        g_count, g_n_built_in, g_count - g_n_built_in);
    return 0;
}

void anchors_cleanup(void) {
    if (!g_initialized) return;
    state_lock();
    if (g_enabled_store) {
        cJSON_Delete(g_enabled_store);
        g_enabled_store = NULL;
    }
    g_count = 0;
    g_n_built_in = 0;
    g_initialized = 0;
    state_unlock();
    g_mutex_clear(&g_state_lock);
}

// ---------------------------------------------------------------------
// 6. Read accessors
// ---------------------------------------------------------------------

size_t anchors_count(void) {
    state_lock();
    size_t n = g_count;
    state_unlock();
    return n;
}

int anchors_get_by_index(size_t index, anchor_t* out) {
    if (!out) return -1;
    state_lock();
    if (index >= g_count) { state_unlock(); return -1; }
    *out = g_anchors[index];
    state_unlock();
    return 0;
}

int anchors_get_by_id(const char* id, anchor_t* out) {
    if (!id || !out) return -1;
    state_lock();
    for (size_t i = 0; i < g_count; i++) {
        if (strcmp(g_anchors[i].id, id) == 0) {
            *out = g_anchors[i];
            state_unlock();
            return 0;
        }
    }
    state_unlock();
    return -1;
}

// Convenience for callers that need a stable pointer to an anchor
// while holding the lock. Returns NULL if not found.
static anchor_t* find_locked(const char* id) {
    for (size_t i = 0; i < g_count; i++)
        if (strcmp(g_anchors[i].id, id) == 0) return &g_anchors[i];
    return NULL;
}

// ---------------------------------------------------------------------
// 7. anchors_resolve_source
// ---------------------------------------------------------------------

// Compute today's UTC instant for a built-in anchor id. `local_day_anchor`
// is converted to local civil date components first.
static int resolve_built_in(const char*  id,
                            time_t       local_day_anchor,
                            time_t*      out) {
    double lat = ACAP_DEVICE_Latitude();
    double lon = ACAP_DEVICE_Longitude();
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        *out = SOLAR_NO_EVENT;
        return 0;
    }

    struct tm tm_local;
    localtime_r(&local_day_anchor, &tm_local);
    int year  = tm_local.tm_year + 1900;
    int month = tm_local.tm_mon  + 1;
    int day   = tm_local.tm_mday;

    // Solar zenith-N events.
    struct { const char* id; double zenith; int rise; } solar_map[] = {
        { "sunrise",      SOLAR_ZENITH_SUNRISE_SUNSET,        1 },
        { "sunset",       SOLAR_ZENITH_SUNRISE_SUNSET,        0 },
        { "civildawn",    SOLAR_ZENITH_CIVIL_TWILIGHT,        1 },
        { "civildusk",    SOLAR_ZENITH_CIVIL_TWILIGHT,        0 },
        { "nauticaldawn", SOLAR_ZENITH_NAUTICAL_TWILIGHT,     1 },
        { "nauticaldusk", SOLAR_ZENITH_NAUTICAL_TWILIGHT,     0 },
        { "astrodawn",    SOLAR_ZENITH_ASTRONOMICAL_TWILIGHT, 1 },
        { "astrodusk",    SOLAR_ZENITH_ASTRONOMICAL_TWILIGHT, 0 },
    };
    for (size_t i = 0; i < sizeof solar_map / sizeof solar_map[0]; i++) {
        if (strcmp(id, solar_map[i].id) == 0) {
            solar_events_t s = {0};
            if (solar_compute(lat, lon, year, month, day,
                              solar_map[i].zenith, &s) != 0) {
                *out = SOLAR_NO_EVENT;
                return 0;
            }
            *out = solar_map[i].rise ? s.sunrise : s.sunset;
            return 0;
        }
    }
    if (strcmp(id, "sunnoon") == 0 || strcmp(id, "sunmidnight") == 0) {
        solar_events_t s = {0};
        if (solar_compute(lat, lon, year, month, day,
                          SOLAR_ZENITH_SUNRISE_SUNSET, &s) != 0) {
            *out = SOLAR_NO_EVENT;
            return 0;
        }
        *out = (strcmp(id, "sunnoon") == 0) ? s.solar_noon : s.solar_midnight;
        return 0;
    }

    // Lunar daily.
    if (strcmp(id, "moonrise") == 0 || strcmp(id, "moonset") == 0 ||
        strcmp(id, "moonnoon") == 0 || strcmp(id, "moonmidnight") == 0) {
        lunar_events_t l = {0};
        if (lunar_compute_daily(lat, lon, year, month, day, &l) != 0) {
            *out = SOLAR_NO_EVENT;
            return 0;
        }
        if (strcmp(id, "moonrise") == 0)     *out = l.moonrise;
        else if (strcmp(id, "moonset") == 0) *out = l.moonset;
        else if (strcmp(id, "moonnoon") == 0)*out = l.lunar_transit;
        else                                 *out = l.lunar_anti_transit;
        return 0;
    }

    // Lunar phases (next-after-now, observer-independent).
    struct { const char* id; lunar_phase_t k; } phase_map[] = {
        { "newmoon",      LUNAR_PHASE_NEW           },
        { "firstquarter", LUNAR_PHASE_FIRST_QUARTER },
        { "fullmoon",     LUNAR_PHASE_FULL          },
        { "lastquarter",  LUNAR_PHASE_LAST_QUARTER  },
    };
    for (size_t i = 0; i < sizeof phase_map / sizeof phase_map[0]; i++) {
        if (strcmp(id, phase_map[i].id) == 0) {
            time_t next = 0;
            if (lunar_next_phase(local_day_anchor, phase_map[i].k, &next) != 0) {
                *out = SOLAR_NO_EVENT;
                return 0;
            }
            *out = next;
            return 0;
        }
    }

    // Seasonal.
    struct { const char* id; seasonal_kind_t k; } season_map[] = {
        { "marchequinox",     SEASONAL_MARCH_EQUINOX     },
        { "junesolstice",     SEASONAL_JUNE_SOLSTICE     },
        { "septemberequinox", SEASONAL_SEPTEMBER_EQUINOX },
        { "decembersolstice", SEASONAL_DECEMBER_SOLSTICE },
    };
    for (size_t i = 0; i < sizeof season_map / sizeof season_map[0]; i++) {
        if (strcmp(id, season_map[i].id) == 0) {
            time_t next = 0;
            if (seasonal_next(local_day_anchor, season_map[i].k, &next) != 0) {
                *out = SOLAR_NO_EVENT;
                return 0;
            }
            *out = next;
            return 0;
        }
    }
    return -1;  // not a known built-in id
}

// Recursive resolution with cycle detection. `visited` is a stack
// buffer of ids currently being resolved. Caller passes depth=0.
//
// On success returns 0 with `*out` set; SOLAR_NO_EVENT means "no event
// today legitimately." On hard error (unknown id, cycle, malformed
// source) returns -1.
//
// Caller should NOT hold g_state_lock when calling this — we acquire
// it internally via anchors_get_by_id.
static int resolve_recursive(const char* id,
                             time_t      local_day_anchor,
                             time_t*     out,
                             const char* visited[],
                             int         depth);

// Maximum recursion depth: 22 built-ins + 64 operator anchors = 86; we
// cap below that to keep the stack buffer small. A legitimate chain of
// "anchor referencing anchor referencing anchor" is unlikely to exceed
// 4-5 hops in real configurations.
#define ANCHORS_RESOLVE_MAX_DEPTH 16

int anchors_resolve_source(const char* event_source,
                           time_t       local_day_anchor,
                           time_t*      out) {
    if (!event_source || !out) return -1;
    const char* visited[ANCHORS_RESOLVE_MAX_DEPTH];
    return resolve_recursive(event_source, local_day_anchor, out, visited, 0);
}

static int resolve_recursive(const char* id,
                             time_t      local_day_anchor,
                             time_t*     out,
                             const char* visited[],
                             int         depth) {
    if (depth >= ANCHORS_RESOLVE_MAX_DEPTH) {
        LOG_WARN("anchors_resolve_source: max recursion depth at id='%s'", id);
        *out = SOLAR_NO_EVENT;
        return 0;
    }
    for (int i = 0; i < depth; i++) {
        if (strcmp(visited[i], id) == 0) {
            LOG_WARN("anchors_resolve_source: cycle detected at id='%s'", id);
            *out = SOLAR_NO_EVENT;
            return 0;
        }
    }
    visited[depth] = id;

    // Try built-in first (fast path).
    {
        time_t t = 0;
        int rc = resolve_built_in(id, local_day_anchor, &t);
        if (rc == 0) { *out = t; return 0; }
    }

    // Try operator anchor.
    anchor_t a;
    if (anchors_get_by_id(id, &a) == 0 && !a.built_in) {
        // Operator anchor — recurse on its source per its kind.
        switch (a.kind) {
            case ANCHOR_KIND_OFFSET: {
                time_t base = 0;
                if (resolve_recursive(a.event_source, local_day_anchor,
                                      &base, visited, depth + 1) != 0) {
                    *out = SOLAR_NO_EVENT;
                    return 0;
                }
                if (base == SOLAR_NO_EVENT) {
                    *out = SOLAR_NO_EVENT;
                    return 0;
                }
                *out = base + (time_t)(a.offset_minutes * 60);
                return 0;
            }
            case ANCHOR_KIND_PAIRED: {
                // Paired anchor as a "source" reduces to its start
                // event (callers typically arm a state-true at start
                // and a state-false at end). For now expose the start
                // time; the timer pattern in timers.c arms both edges.
                time_t base = 0;
                if (resolve_recursive(a.start_event, local_day_anchor,
                                      &base, visited, depth + 1) != 0) {
                    *out = SOLAR_NO_EVENT;
                    return 0;
                }
                if (base == SOLAR_NO_EVENT) { *out = SOLAR_NO_EVENT; return 0; }
                *out = base + (time_t)(a.start_offset_minutes * 60);
                return 0;
            }
            case ANCHOR_KIND_THRESHOLD:
                // Threshold anchors don't have a deterministic "today"
                // time outside the look-ahead pipeline. Treat as no
                // event for the source-resolution path; the threshold
                // scheduler in timers.c is what arms their fires.
                *out = SOLAR_NO_EVENT;
                return 0;
        }
    }

    // Try calendar entry.
    {
        time_t next = 0;
        int rc = calendar_next_occurrence(id, local_day_anchor, &next, NULL);
        if (rc == 0) {
            // Only treat as a "today" hit if next falls within the same
            // local day.
            if (next == (time_t)-1) { *out = SOLAR_NO_EVENT; return 0; }
            struct tm a_tm, b_tm;
            localtime_r(&local_day_anchor, &a_tm);
            localtime_r(&next, &b_tm);
            if (a_tm.tm_year == b_tm.tm_year && a_tm.tm_mon == b_tm.tm_mon &&
                a_tm.tm_mday == b_tm.tm_mday) {
                *out = next;
                return 0;
            }
            *out = SOLAR_NO_EVENT;
            return 0;
        }
    }

    LOG_WARN("anchors_resolve_source: unknown id '%s'", id);
    return -1;
}

// ---------------------------------------------------------------------
// 8. CRUD mutators
// ---------------------------------------------------------------------

// Persist current operator slice atomically. Caller must hold lock.
static int persist_operator_slice_locked(void) {
    cJSON* arr = operator_anchors_to_json_array();
    if (!arr) return ANCHORS_ERR_INTERNAL;
    int rc = persistence_write_atomic("localdata/anchors.json", arr,
                                      validate_anchors_file, NULL);
    cJSON_Delete(arr);
    return (rc == 0) ? ANCHORS_OK : ANCHORS_ERR_PERSIST;
}

// The state-flag for an anchor's AXEvent topic. See contract §1.8 for
// the stateful-vs-pulse mapping.
static int anchor_is_stateful(const anchor_t* a) {
    if (a->kind == ANCHOR_KIND_PAIRED) return 1;
    if (a->kind == ANCHOR_KIND_OFFSET && a->duration_minutes > 0) return 1;
    return 0;
}

int anchors_create(const anchor_t* in) {
    if (!in) return ANCHORS_ERR_INVALID;
    int rc = validate_anchor_fields(in);
    if (rc != ANCHORS_OK) return rc;
    rc = validate_anchor_dependencies(in);
    if (rc != ANCHORS_OK) return rc;

    state_lock();
    if (g_count - g_n_built_in >= ANCHORS_OPERATOR_MAX) {
        state_unlock();
        return ANCHORS_ERR_FULL;
    }
    if (anchor_id_taken_locked(in->id)) {
        state_unlock();
        return ANCHORS_ERR_DUPLICATE;
    }
    state_unlock();
    // Calendar lookup must happen without our lock held (it takes its
    // own lock).
    calendar_entry_t tmp;
    if (calendar_get_by_id(in->id, &tmp) == 0) return ANCHORS_ERR_DUPLICATE;

    state_lock();
    // Snapshot pre-state for rollback.
    size_t prev_count = g_count;

    anchor_t* slot = &g_anchors[g_count];
    *slot = *in;
    slot->built_in = 0;
    slot->enabled  = enabled_lookup_locked(in->id);
    g_count++;

    if (persist_operator_slice_locked() != ANCHORS_OK) {
        g_count = prev_count;
        state_unlock();
        return ANCHORS_ERR_PERSIST;
    }

    // Reconciliation: declare the new AXEvent topic.
    // ACAP_EVENTS_Add_Event returns the declarationID (non-zero) on
    // success, 0 on failure. The check below treats zero as failure.
    if (ACAP_EVENTS_Add_Event(slot->id, slot->name,
                              anchor_is_stateful(slot)) == 0) {
        // Roll back: revert the in-memory list and persistence.
        g_count = prev_count;
        (void)persist_operator_slice_locked();
        state_unlock();
        return ANCHORS_ERR_REGISTER;
    }
    state_unlock();

    trigger_recompute();
    return ANCHORS_OK;
}

int anchors_update(const anchor_t* in) {
    if (!in) return ANCHORS_ERR_INVALID;
    int rc = validate_anchor_fields(in);
    if (rc != ANCHORS_OK) return rc;
    rc = validate_anchor_dependencies(in);
    if (rc != ANCHORS_OK) return rc;

    state_lock();
    anchor_t* existing = find_locked(in->id);
    if (!existing) { state_unlock(); return ANCHORS_ERR_NOT_FOUND; }
    if (existing->built_in) { state_unlock(); return ANCHORS_ERR_BUILTIN; }

    anchor_t prev = *existing;
    int name_only = (strcmp(prev.name, in->name) != 0)
                 && (prev.kind == in->kind)
                 && (prev.offset_minutes == in->offset_minutes)
                 && (prev.duration_minutes == in->duration_minutes)
                 && (prev.start_offset_minutes == in->start_offset_minutes)
                 && (prev.end_offset_minutes == in->end_offset_minutes)
                 && (strcmp(prev.event_source, in->event_source) == 0)
                 && (strcmp(prev.start_event, in->start_event) == 0)
                 && (strcmp(prev.end_event, in->end_event) == 0)
                 && (prev.metric == in->metric)
                 && (prev.op == in->op)
                 && (prev.value == in->value);

    *existing = *in;
    existing->built_in = 0;
    existing->enabled  = enabled_lookup_locked(in->id);

    if (persist_operator_slice_locked() != ANCHORS_OK) {
        *existing = prev;
        state_unlock();
        return ANCHORS_ERR_PERSIST;
    }

    // FR-8.5: re-declare with new NiceName + state. The vendored
    // ACAP_EVENTS layer accepts redeclaration without a prior remove
    // for stateful->pulse transitions, but to be safe we Remove +
    // Add_Event when shape changes. Name-only edits use the same
    // pattern (matches apply_seasonal_labels).
    ACAP_EVENTS_Remove_Event(existing->id);
    if (ACAP_EVENTS_Add_Event(existing->id, existing->name,
                              anchor_is_stateful(existing)) == 0) {
        *existing = prev;
        (void)persist_operator_slice_locked();
        state_unlock();
        return ANCHORS_ERR_REGISTER;
    }
    state_unlock();

    if (!name_only) trigger_recompute();
    return ANCHORS_OK;
}

int anchors_delete(const char* id) {
    if (!is_valid_id(id)) return ANCHORS_ERR_INVALID;

    state_lock();
    anchor_t* existing = find_locked(id);
    if (!existing) { state_unlock(); return ANCHORS_ERR_NOT_FOUND; }
    if (existing->built_in) { state_unlock(); return ANCHORS_ERR_BUILTIN; }

    size_t idx = (size_t)(existing - g_anchors);
    anchor_t prev = *existing;

    // Compact the array.
    for (size_t i = idx; i + 1 < g_count; i++)
        g_anchors[i] = g_anchors[i + 1];
    g_count--;

    if (persist_operator_slice_locked() != ANCHORS_OK) {
        // Rollback: re-insert.
        for (size_t i = g_count; i > idx; i--)
            g_anchors[i] = g_anchors[i - 1];
        g_anchors[idx] = prev;
        g_count++;
        state_unlock();
        return ANCHORS_ERR_PERSIST;
    }

    // Reconciliation: undeclare. We don't fail the delete if the
    // remove returns non-zero — the topic may simply already be gone
    // from a prior aborted attempt. Log only.
    if (ACAP_EVENTS_Remove_Event(prev.id) != 0)
        LOG_WARN("anchors_delete: ACAP_EVENTS_Remove_Event('%s') returned non-zero",
                 prev.id);
    state_unlock();

    trigger_recompute();
    return ANCHORS_OK;
}

int anchors_set_enabled(const char* id, int enabled) {
    if (!is_valid_id(id)) return ANCHORS_ERR_INVALID;

    state_lock();
    // Verify the id refers to a known schedule (anchor or calendar).
    int known = (find_locked(id) != NULL);
    state_unlock();
    if (!known) {
        calendar_entry_t tmp;
        if (calendar_get_by_id(id, &tmp) != 0)
            return ANCHORS_ERR_NOT_FOUND;
    }

    state_lock();
    if (!g_enabled_store) g_enabled_store = cJSON_CreateObject();

    // Snapshot pre-state for rollback.
    cJSON* prev_item = cJSON_GetObjectItem(g_enabled_store, id);
    int had_prev = (prev_item != NULL);
    int prev_val = had_prev ? (cJSON_IsTrue(prev_item) ? 1 : 0) : 1;

    // Replace or add. cJSON_ReplaceItemInObject doesn't add if missing,
    // so we delete + add.
    if (had_prev) cJSON_DeleteItemFromObject(g_enabled_store, id);
    cJSON_AddBoolToObject(g_enabled_store, id, enabled ? 1 : 0);

    if (save_enabled_store_locked() != 0) {
        // Roll back.
        cJSON_DeleteItemFromObject(g_enabled_store, id);
        if (had_prev)
            cJSON_AddBoolToObject(g_enabled_store, id, prev_val ? 1 : 0);
        state_unlock();
        return ANCHORS_ERR_PERSIST;
    }

    // Mirror into in-memory anchor flag (if it's an anchor — calendar
    // entries query the store directly via anchors_is_enabled).
    anchor_t* a = find_locked(id);
    if (a) a->enabled = enabled ? 1 : 0;
    state_unlock();

    LOG("anchors_set_enabled: id='%s' enabled=%d", id, enabled ? 1 : 0);
    return ANCHORS_OK;
}

// Returns 1 iff `id` resolves either in the global namespace (built-in
// anchors + calendar) or within the supplied batch. Used by
// anchors_replace_all so an anchor that references another anchor in
// the same batch validates correctly.
static int id_resolves_in_batch(const char* id,
                                const anchor_t* batch, size_t batch_count) {
    if (!id) return 0;
    // Built-in only — operator anchors are about to be replaced and
    // therefore not authoritative.
    state_lock();
    for (size_t i = 0; i < g_n_built_in; i++) {
        if (strcmp(g_anchors[i].id, id) == 0) { state_unlock(); return 1; }
    }
    state_unlock();
    if (calendar_id_exists(id)) return 1;
    for (size_t i = 0; i < batch_count; i++) {
        if (strcmp(batch[i].id, id) == 0) return 1;
    }
    return 0;
}

int anchors_replace_all(const anchor_t* operator_anchors, size_t count) {
    if (!operator_anchors && count > 0) return ANCHORS_ERR_INVALID;
    if (count > ANCHORS_OPERATOR_MAX)   return ANCHORS_ERR_FULL;

    // Validate all entries first (all-or-nothing).
    for (size_t i = 0; i < count; i++) {
        int rc = validate_anchor_fields(&operator_anchors[i]);
        if (rc != ANCHORS_OK) return rc;
        // Cross-namespace: must not collide with a built-in or any
        // calendar entry. (Duplicates within the batch caught below.)
        for (size_t j = 0; j < i; j++) {
            if (strcmp(operator_anchors[i].id, operator_anchors[j].id) == 0)
                return ANCHORS_ERR_DUPLICATE;
        }
        // Dependency-existence check (FR-7.1). Allows references
        // within the batch.
        const anchor_t* a = &operator_anchors[i];
        if (a->kind == ANCHOR_KIND_OFFSET) {
            if (!id_resolves_in_batch(a->event_source, operator_anchors, count))
                return ANCHORS_ERR_DEP;
        } else if (a->kind == ANCHOR_KIND_PAIRED) {
            if (!id_resolves_in_batch(a->start_event, operator_anchors, count))
                return ANCHORS_ERR_DEP;
            if (!id_resolves_in_batch(a->end_event, operator_anchors, count))
                return ANCHORS_ERR_DEP;
        }
    }
    state_lock();
    for (size_t i = 0; i < count; i++) {
        // Built-in collision check.
        for (size_t j = 0; j < g_n_built_in; j++) {
            if (strcmp(operator_anchors[i].id, g_anchors[j].id) == 0) {
                state_unlock();
                return ANCHORS_ERR_DUPLICATE;
            }
        }
    }
    state_unlock();
    for (size_t i = 0; i < count; i++) {
        calendar_entry_t tmp;
        if (calendar_get_by_id(operator_anchors[i].id, &tmp) == 0)
            return ANCHORS_ERR_DUPLICATE;
    }

    state_lock();
    // Snapshot for rollback.
    anchor_t   prev_slice[ANCHORS_OPERATOR_MAX];
    size_t     prev_op_count = g_count - g_n_built_in;
    for (size_t i = 0; i < prev_op_count; i++)
        prev_slice[i] = g_anchors[g_n_built_in + i];

    // Tear down old AXEvent declarations.
    for (size_t i = 0; i < prev_op_count; i++)
        (void)ACAP_EVENTS_Remove_Event(prev_slice[i].id);

    // Install new slice.
    g_count = g_n_built_in;
    for (size_t i = 0; i < count; i++) {
        anchor_t a = operator_anchors[i];
        a.built_in = 0;
        a.enabled  = enabled_lookup_locked(a.id);
        g_anchors[g_count++] = a;
    }

    if (persist_operator_slice_locked() != ANCHORS_OK) {
        // Roll back.
        g_count = g_n_built_in;
        for (size_t i = 0; i < prev_op_count; i++) {
            g_anchors[g_count++] = prev_slice[i];
            ACAP_EVENTS_Add_Event(prev_slice[i].id, prev_slice[i].name,
                                  anchor_is_stateful(&prev_slice[i]));
        }
        state_unlock();
        return ANCHORS_ERR_PERSIST;
    }

    // Declare the new topics.
    for (size_t i = 0; i < count; i++) {
        anchor_t* a = &g_anchors[g_n_built_in + i];
        if (ACAP_EVENTS_Add_Event(a->id, a->name, anchor_is_stateful(a)) == 0) {
            LOG_WARN("anchors_replace_all: ACAP_EVENTS_Add_Event('%s') failed",
                     a->id);
            // Best-effort: log and continue. Test agent should assert
            // that the persist + in-memory state are still consistent.
        }
    }
    state_unlock();

    trigger_recompute();
    return ANCHORS_OK;
}
