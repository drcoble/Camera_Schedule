// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// M2 application entry point. Boots the GLib main loop, initializes
// the vendored ACAP framework, registers FastCGI endpoints, declares
// sunrise/sunset event topics (via settings/events.json that ACAP()
// reads at boot), and arms the daily-recompute scheduler from
// timers.c.
//
// Endpoints:
//   GET  /local/camera_schedule/about
//        → {name, version, arch}
//   GET  /local/camera_schedule/location
//        → {lat, lon}
//   POST /local/camera_schedule/location
//        body: {"lat": <number>, "lon": <number>}
//        → updated {lat, lon}; writes through to the camera's
//          geolocation service (per DL-07) and triggers a recompute.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <glib.h>
#include <glib-unix.h>
#include <axsdk/axparameter.h>

#include "acap/ACAP.h"
#include "acap/cJSON.h"
#include "anchors.h"
#include "calendar.h"
#include "log.h"
#include "persistence.h"
#include "status.h"
#include "timers.h"
#include "astro/solar.h"
#include "astro/lunar.h"
#include "astro/seasonal.h"

#define APP_PACKAGE "camera_schedule"
#define APP_VERSION "0.7.0"

#if defined(__aarch64__)
#define APP_ARCH "aarch64"
#elif defined(__arm__)
#define APP_ARCH "armv7hf"
#else
#define APP_ARCH "unknown"
#endif

// Storage for the runtime LOG_DBG gate (FR-13.4). The header-only
// macros in log.h read this flag through the LOG_DBG conditional;
// AXParameter callback + POST /debug flip it.
int g_debug_logging_enabled = 0;

// AXParameter handle. Owned by main(); freed in cleanup. Created lazily
// after ACAP() so the package's parameter namespace exists before the
// first add_or_get call.
static AXParameter* g_axparam = NULL;

// AXParameter scalar settings (FR-12.2). Cached in-memory; the canonical
// store is the AXParameter system, mirrored here so /status and /debug
// can return them without an extra round-trip through param.cgi.
static int  g_param_lookahead_days       = 7;
static char g_param_event_name_prefix[33] = "";
static int  g_param_poll_interval_seconds = 60;

static GMainLoop* main_loop = NULL;

static gboolean signal_handler(gpointer user_data) {
    (void)user_data;
    LOG("Signal received, shutting down");
    if (main_loop) g_main_loop_quit(main_loop);
    return G_SOURCE_REMOVE;
}

// Forward declaration — apply_seasonal_labels is defined below the
// HTTP handlers but referenced from HTTP_Endpoint_Location.
static void apply_seasonal_labels(double lat);

// ---- HTTP handlers ------------------------------------------------

static void HTTP_Endpoint_About(const ACAP_HTTP_Response response,
                                const ACAP_HTTP_Request  request) {
    (void)request;
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name",    "Camera_Schedule");
    cJSON_AddStringToObject(body, "version", APP_VERSION);
    cJSON_AddStringToObject(body, "arch",    APP_ARCH);
    ACAP_HTTP_Respond_JSON(response, body);
    cJSON_Delete(body);
}

// Build a {lat, lon} JSON object reflecting the camera's current
// geolocation. Caller frees.
static cJSON* current_location_json(void) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "lat", ACAP_DEVICE_Latitude());
    cJSON_AddNumberToObject(o, "lon", ACAP_DEVICE_Longitude());
    return o;
}

static void HTTP_Endpoint_Location(const ACAP_HTTP_Response response,
                                   const ACAP_HTTP_Request  request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method) {
        ACAP_HTTP_Respond_Error(response, 400, "Invalid request method");
        return;
    }

    if (strcmp(method, "GET") == 0) {
        cJSON* body = current_location_json();
        ACAP_HTTP_Respond_JSON(response, body);
        cJSON_Delete(body);
        return;
    }

    if (strcmp(method, "POST") == 0) {
        if (!request->postData) {
            ACAP_HTTP_Respond_Error(response, 400, "Missing POST body");
            return;
        }
        cJSON* in = cJSON_Parse(request->postData);
        if (!in) {
            ACAP_HTTP_Respond_Error(response, 400, "Body is not valid JSON");
            return;
        }

        cJSON* lat_o = cJSON_GetObjectItem(in, "lat");
        cJSON* lon_o = cJSON_GetObjectItem(in, "lon");
        if (!cJSON_IsNumber(lat_o) || !cJSON_IsNumber(lon_o)) {
            cJSON_Delete(in);
            ACAP_HTTP_Respond_Error(response, 400,
                "Body must include numeric `lat` and `lon`");
            return;
        }

        double lat = lat_o->valuedouble;
        double lon = lon_o->valuedouble;
        cJSON_Delete(in);

        if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
            ACAP_HTTP_Respond_Error(response, 400,
                "lat must be in [-90,90] and lon in [-180,180]");
            return;
        }

        if (!ACAP_DEVICE_Set_Location(lat, lon)) {
            LOG_WARN("Failed to write geolocation lat=%f lon=%f", lat, lon);
            ACAP_HTTP_Respond_Error(response, 500,
                "Camera rejected the geolocation update");
            return;
        }

        LOG("Geolocation updated to lat=%f lon=%f; recomputing events", lat, lon);
        apply_seasonal_labels(lat);
        timers_recompute_now(RECOMPUTE_TRIGGER_LOCATION_CHANGE);

        cJSON* body = current_location_json();
        ACAP_HTTP_Respond_JSON(response, body);
        cJSON_Delete(body);
        return;
    }

    ACAP_HTTP_Respond_Error(response, 405, "Method Not Allowed");
}

// ---- M6 helpers: error envelope, ISO time formatting --------------

// The vendored ACAP_HTTP_Respond_Error emits text/plain. The M6
// contract requires a JSON envelope `{"error":"<tag>","message":"..."}`.
// This helper writes the contract envelope with the right HTTP status
// line and content type.
static void respond_json_error(ACAP_HTTP_Response response, int code,
                               const char* tag, const char* message) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error",   tag ? tag : "error");
    cJSON_AddStringToObject(o, "message", message ? message : "");
    char* body = cJSON_PrintUnformatted(o);
    const char* phrase = (code < 500) ? "Client Error" : "Server Error";
    if (body) {
        ACAP_HTTP_Respond_String(response,
            "Status: %d %s\r\n"
            "Content-Type: application/json\r\n"
            "\r\n"
            "%s",
            code, phrase, body);
        free(body);
    }
    cJSON_Delete(o);
    LOG_WARN("HTTP %d %s: %s", code, tag ? tag : "error", message ? message : "");
}

// Map an ANCHORS_ERR_* code to a (status, tag, message) tuple. Returns
// the HTTP status; writes tag+message via the out-pointers. Used by the
// anchors and calendar handlers.
static int map_anchors_err(int rc, const char** tag_out, const char** msg_out) {
    switch (rc) {
        case ANCHORS_OK:            *tag_out = "ok";                    *msg_out = "";                                     return 200;
        case ANCHORS_ERR_NOT_FOUND: *tag_out = "not_found";             *msg_out = "anchor not found";                     return 404;
        case ANCHORS_ERR_BUILTIN:   *tag_out = "builtin_immutable";     *msg_out = "built-in anchors are not modifiable";  return 403;
        case ANCHORS_ERR_INVALID:   *tag_out = "validation_error";      *msg_out = "invalid anchor field";                 return 400;
        case ANCHORS_ERR_DUPLICATE: *tag_out = "id_conflict";           *msg_out = "id collides with another schedule";    return 409;
        case ANCHORS_ERR_FULL:      *tag_out = "operator_cap";          *msg_out = "operator anchor cap reached";          return 413;
        case ANCHORS_ERR_DEP:       *tag_out = "missing_dependency";    *msg_out = "referenced source not found";          return 422;
        case ANCHORS_ERR_PERSIST:   *tag_out = "persist_failed";        *msg_out = "atomic write to localdata failed";     return 500;
        case ANCHORS_ERR_REGISTER:  *tag_out = "axevent_failed";        *msg_out = "ACAP event declaration failed";        return 500;
        case ANCHORS_ERR_INTERNAL:  *tag_out = "internal_error";        *msg_out = "unexpected internal error";            return 500;
        default:                    *tag_out = "internal_error";        *msg_out = "unknown error";                        return 500;
    }
}

// Format a time_t as ISO-8601 in the camera's local timezone with a
// `+HH:MM` offset. `out` is a 32-byte buffer (the longest valid ISO-8601
// is 25 chars + NUL).
static void format_iso_local(time_t t, char* out, size_t out_len) {
    if (t <= 0) { if (out_len) out[0] = '\0'; return; }
    struct tm tm;
    localtime_r(&t, &tm);
    char raw[64];
    if (strftime(raw, sizeof raw, "%Y-%m-%dT%H:%M:%S%z", &tm) == 0) {
        out[0] = '\0';
        return;
    }
    // strftime emits %z as "+HHMM"; insert the colon to make ISO-8601.
    size_t len = strlen(raw);
    if (len >= 5 && (raw[len - 5] == '+' || raw[len - 5] == '-')) {
        snprintf(out, out_len, "%.*s%c%.2s:%.2s",
                 (int)(len - 5), raw, raw[len - 5],
                 raw + len - 4, raw + len - 2);
    } else {
        snprintf(out, out_len, "%s", raw);
    }
}

static void format_iso_utc(time_t t, char* out, size_t out_len) {
    if (t <= 0) { out[0] = '\0'; return; }
    struct tm tm;
    gmtime_r(&t, &tm);
    if (strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) out[0] = '\0';
}

// ---- Anchors endpoint ---------------------------------------------

// Build a snapshot of the full anchor list as the contract §1.1 shape.
static cJSON* build_anchors_response(void) {
    cJSON* root = cJSON_CreateObject();
    cJSON* built_in = cJSON_AddArrayToObject(root, "built_in");
    cJSON* operator_arr = cJSON_AddArrayToObject(root, "operator");
    size_t n = anchors_count();
    for (size_t i = 0; i < n; i++) {
        anchor_t a;
        if (anchors_get_by_index(i, &a) != 0) continue;
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id",   a.id);
        cJSON_AddStringToObject(o, "name", a.name);
        const char* kind_s =
            (a.kind == ANCHOR_KIND_OFFSET)    ? "offset" :
            (a.kind == ANCHOR_KIND_PAIRED)    ? "paired" :
            (a.kind == ANCHOR_KIND_THRESHOLD) ? "threshold" : "offset";
        cJSON_AddStringToObject(o, "kind", kind_s);
        cJSON_AddBoolToObject(o, "built_in", a.built_in ? 1 : 0);
        // Authoritative enable state lives in the schedule_enabled
        // toggle store (DL-18); always consult it here for symmetry
        // with calendar entries.
        cJSON_AddBoolToObject(o, "enabled", anchors_is_enabled(a.id));
        switch (a.kind) {
            case ANCHOR_KIND_OFFSET:
                cJSON_AddStringToObject(o, "event_source",     a.event_source);
                cJSON_AddNumberToObject(o, "offset_minutes",   a.offset_minutes);
                cJSON_AddNumberToObject(o, "duration_minutes", a.duration_minutes);
                break;
            case ANCHOR_KIND_PAIRED:
                cJSON_AddStringToObject(o, "start_event",          a.start_event);
                cJSON_AddNumberToObject(o, "start_offset_minutes", a.start_offset_minutes);
                cJSON_AddStringToObject(o, "end_event",            a.end_event);
                cJSON_AddNumberToObject(o, "end_offset_minutes",   a.end_offset_minutes);
                break;
            case ANCHOR_KIND_THRESHOLD: {
                cJSON_AddStringToObject(o, "metric", "moon_illumination");
                const char* op_s =
                    (a.op == ANCHOR_OP_GE) ? "ge" :
                    (a.op == ANCHOR_OP_LE) ? "le" :
                    (a.op == ANCHOR_OP_GT) ? "gt" : "lt";
                cJSON_AddStringToObject(o, "op", op_s);
                cJSON_AddNumberToObject(o, "value", a.value);
                break;
            }
        }
        cJSON_AddItemToArray(a.built_in ? built_in : operator_arr, o);
    }
    return root;
}

// Parse one anchor object from the wire shape (§2.1 / §2.2). Returns 0
// on success, -1 on malformed input. Does NOT validate ranges — the
// anchors_create / anchors_update entry points do that.
static int anchor_from_wire(cJSON* obj, anchor_t* out) {
    if (!cJSON_IsObject(obj) || !out) return -1;
    memset(out, 0, sizeof *out);
    out->built_in = 0;
    out->enabled  = 1;

    cJSON* id_v   = cJSON_GetObjectItem(obj, "id");
    cJSON* name_v = cJSON_GetObjectItem(obj, "name");
    cJSON* kind_v = cJSON_GetObjectItem(obj, "kind");
    if (!cJSON_IsString(id_v) || !cJSON_IsString(name_v) || !cJSON_IsString(kind_v))
        return -1;
    if (strlen(id_v->valuestring)   > ANCHORS_ID_MAX)   return -1;
    if (strlen(name_v->valuestring) > ANCHORS_NAME_MAX) return -1;
    snprintf(out->id,   sizeof out->id,   "%s", id_v->valuestring);
    snprintf(out->name, sizeof out->name, "%s", name_v->valuestring);

    if (strcmp(kind_v->valuestring, "offset") == 0) {
        out->kind = ANCHOR_KIND_OFFSET;
        cJSON* es = cJSON_GetObjectItem(obj, "event_source");
        cJSON* om = cJSON_GetObjectItem(obj, "offset_minutes");
        cJSON* dm = cJSON_GetObjectItem(obj, "duration_minutes");
        if (!cJSON_IsString(es) || !cJSON_IsNumber(om) || !cJSON_IsNumber(dm))
            return -1;
        if (strlen(es->valuestring) > ANCHORS_ID_MAX) return -1;
        snprintf(out->event_source, sizeof out->event_source, "%s", es->valuestring);
        out->offset_minutes   = (int)om->valuedouble;
        out->duration_minutes = (int)dm->valuedouble;
    } else if (strcmp(kind_v->valuestring, "paired") == 0) {
        out->kind = ANCHOR_KIND_PAIRED;
        cJSON* se = cJSON_GetObjectItem(obj, "start_event");
        cJSON* sm = cJSON_GetObjectItem(obj, "start_offset_minutes");
        cJSON* ee = cJSON_GetObjectItem(obj, "end_event");
        cJSON* em = cJSON_GetObjectItem(obj, "end_offset_minutes");
        if (!cJSON_IsString(se) || !cJSON_IsString(ee) ||
            !cJSON_IsNumber(sm) || !cJSON_IsNumber(em)) return -1;
        if (strlen(se->valuestring) > ANCHORS_ID_MAX) return -1;
        if (strlen(ee->valuestring) > ANCHORS_ID_MAX) return -1;
        snprintf(out->start_event, sizeof out->start_event, "%s", se->valuestring);
        snprintf(out->end_event,   sizeof out->end_event,   "%s", ee->valuestring);
        out->start_offset_minutes = (int)sm->valuedouble;
        out->end_offset_minutes   = (int)em->valuedouble;
    } else if (strcmp(kind_v->valuestring, "threshold") == 0) {
        out->kind = ANCHOR_KIND_THRESHOLD;
        cJSON* m = cJSON_GetObjectItem(obj, "metric");
        cJSON* op_v = cJSON_GetObjectItem(obj, "op");
        cJSON* v = cJSON_GetObjectItem(obj, "value");
        if (!cJSON_IsString(m) || !cJSON_IsString(op_v) || !cJSON_IsNumber(v))
            return -1;
        if (strcmp(m->valuestring, "moon_illumination") != 0) return -1;
        out->metric = ANCHOR_METRIC_MOON_ILLUMINATION;
        if      (strcmp(op_v->valuestring, "ge") == 0) out->op = ANCHOR_OP_GE;
        else if (strcmp(op_v->valuestring, "le") == 0) out->op = ANCHOR_OP_LE;
        else if (strcmp(op_v->valuestring, "gt") == 0) out->op = ANCHOR_OP_GT;
        else if (strcmp(op_v->valuestring, "lt") == 0) out->op = ANCHOR_OP_LT;
        else return -1;
        out->value = v->valuedouble;
    } else {
        return -1;
    }
    return 0;
}

static void HTTP_Endpoint_Anchors(const ACAP_HTTP_Response response,
                                  const ACAP_HTTP_Request  request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method) {
        respond_json_error(response, 400, "bad_request", "missing method");
        return;
    }

    if (strcmp(method, "GET") == 0) {
        cJSON* body = build_anchors_response();
        ACAP_HTTP_Respond_JSON(response, body);
        cJSON_Delete(body);
        return;
    }

    if (strcmp(method, "POST") == 0) {
        if (!request->postData) {
            respond_json_error(response, 400, "bad_request", "missing body");
            return;
        }
        cJSON* in = cJSON_Parse(request->postData);
        if (!in) {
            respond_json_error(response, 400, "bad_request", "body is not valid JSON");
            return;
        }
        cJSON* mode_v = cJSON_GetObjectItem(in, "mode");
        if (!cJSON_IsString(mode_v)) {
            cJSON_Delete(in);
            respond_json_error(response, 400, "bad_request", "missing 'mode'");
            return;
        }
        if (strcmp(mode_v->valuestring, "upsert") == 0) {
            cJSON* a_obj = cJSON_GetObjectItem(in, "anchor");
            anchor_t a;
            if (!a_obj || anchor_from_wire(a_obj, &a) != 0) {
                cJSON_Delete(in);
                respond_json_error(response, 400, "validation_error",
                                   "invalid anchor object");
                return;
            }
            // Decide create vs update from current state.
            anchor_t existing;
            int found = (anchors_get_by_id(a.id, &existing) == 0);
            int rc;
            if (found && existing.built_in) {
                cJSON_Delete(in);
                respond_json_error(response, 403, "builtin_immutable",
                                   "built-in anchors are not modifiable");
                return;
            }
            rc = found ? anchors_update(&a) : anchors_create(&a);
            cJSON_Delete(in);
            if (rc != ANCHORS_OK) {
                const char* tag; const char* msg;
                int code = map_anchors_err(rc, &tag, &msg);
                respond_json_error(response, code, tag, msg);
                return;
            }
            cJSON* body = build_anchors_response();
            ACAP_HTTP_Respond_JSON(response, body);
            cJSON_Delete(body);
            return;
        }
        if (strcmp(mode_v->valuestring, "replace_all") == 0) {
            cJSON* arr = cJSON_GetObjectItem(in, "anchors");
            if (!cJSON_IsArray(arr)) {
                cJSON_Delete(in);
                respond_json_error(response, 400, "bad_request",
                                   "missing 'anchors' array");
                return;
            }
            int n = cJSON_GetArraySize(arr);
            if (n > (int)ANCHORS_OPERATOR_MAX) {
                cJSON_Delete(in);
                respond_json_error(response, 413, "operator_cap",
                                   "batch exceeds 64-anchor cap");
                return;
            }
            anchor_t batch[ANCHORS_OPERATOR_MAX];
            for (int i = 0; i < n; i++) {
                cJSON* el = cJSON_GetArrayItem(arr, i);
                if (anchor_from_wire(el, &batch[i]) != 0) {
                    cJSON_Delete(in);
                    char msg[64];
                    snprintf(msg, sizeof msg, "anchor %d malformed", i);
                    respond_json_error(response, 400, "validation_error", msg);
                    return;
                }
            }
            cJSON_Delete(in);
            int rc = anchors_replace_all(batch, (size_t)n);
            if (rc != ANCHORS_OK) {
                const char* tag; const char* msg;
                int code = map_anchors_err(rc, &tag, &msg);
                respond_json_error(response, code, tag, msg);
                return;
            }
            cJSON* body = build_anchors_response();
            ACAP_HTTP_Respond_JSON(response, body);
            cJSON_Delete(body);
            return;
        }
        cJSON_Delete(in);
        respond_json_error(response, 400, "bad_request", "unknown 'mode'");
        return;
    }

    if (strcmp(method, "DELETE") == 0) {
        const char* id = ACAP_HTTP_Request_Param(request, "id");
        if (!id) {
            respond_json_error(response, 400, "bad_request", "missing 'id' query param");
            return;
        }
        char id_buf[ANCHORS_ID_MAX + 1];
        snprintf(id_buf, sizeof id_buf, "%s", id);
        free((void*)id);  // ACAP_HTTP_Request_Param malloc'd
        int rc = anchors_delete(id_buf);
        if (rc != ANCHORS_OK) {
            const char* tag; const char* msg;
            int code = map_anchors_err(rc, &tag, &msg);
            respond_json_error(response, code, tag, msg);
            return;
        }
        cJSON* body = build_anchors_response();
        ACAP_HTTP_Respond_JSON(response, body);
        cJSON_Delete(body);
        return;
    }

    respond_json_error(response, 405, "method_not_allowed", "use GET, POST, or DELETE");
}

// ---- Calendar endpoint --------------------------------------------

static cJSON* build_calendar_response(void) {
    cJSON* root = cJSON_CreateObject();
    cJSON* arr  = cJSON_AddArrayToObject(root, "entries");
    size_t n = calendar_count();
    for (size_t i = 0; i < n; i++) {
        calendar_entry_t e;
        if (calendar_get_by_index(i, &e) != 0) continue;
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id",   e.id);
        cJSON_AddStringToObject(o, "name", e.name);
        const char* kind_s =
            (e.kind == CALENDAR_KIND_SINGLE_DATE) ? "single_date" :
            (e.kind == CALENDAR_KIND_DATE_RANGE)  ? "date_range"  :
            "annual";
        cJSON_AddStringToObject(o, "kind", kind_s);
        cJSON_AddStringToObject(o, "time_mode",
            e.time_mode == CALENDAR_TIME_ALL_DAY ? "all_day" : "specific");
        if (e.time_mode == CALENDAR_TIME_SPECIFIC) {
            char buf[16];
            int s = e.time_of_day_seconds;
            snprintf(buf, sizeof buf, "%02d:%02d:%02d",
                     s / 3600, (s % 3600) / 60, s % 60);
            cJSON_AddStringToObject(o, "time_of_day", buf);
        }
        char date_buf[16];
        if (e.kind == CALENDAR_KIND_ANNUAL) {
            snprintf(date_buf, sizeof date_buf, "%04d-%02d-%02d",
                     2000, e.start_date.month, e.start_date.day);
        } else {
            snprintf(date_buf, sizeof date_buf, "%04d-%02d-%02d",
                     e.start_date.year, e.start_date.month, e.start_date.day);
        }
        cJSON_AddStringToObject(o, "start_date", date_buf);
        if (e.kind == CALENDAR_KIND_DATE_RANGE) {
            snprintf(date_buf, sizeof date_buf, "%04d-%02d-%02d",
                     e.end_date.year, e.end_date.month, e.end_date.day);
            cJSON_AddStringToObject(o, "end_date", date_buf);
        }
        cJSON_AddStringToObject(o, "notes", e.notes);
        // Authoritative enable state lives in localdata/schedule_enabled.json
        // per DL-18; the in-memory `e.enabled` is a snapshot taken at
        // calendar_init time and goes stale after a toggle. Always
        // consult anchors_is_enabled (the firing-path gate) here.
        cJSON_AddBoolToObject(o, "enabled", anchors_is_enabled(e.id));
        cJSON_AddItemToArray(arr, o);
    }
    return root;
}

// Parse a calendar entry from wire shape into a calendar_entry_t.
// Returns 0 on success, -1 on malformed input.
static int calendar_from_wire(cJSON* obj, calendar_entry_t* out) {
    if (!cJSON_IsObject(obj) || !out) return -1;
    memset(out, 0, sizeof *out);
    out->enabled = 1;

    cJSON* id_v   = cJSON_GetObjectItem(obj, "id");
    cJSON* name_v = cJSON_GetObjectItem(obj, "name");
    cJSON* kind_v = cJSON_GetObjectItem(obj, "kind");
    cJSON* tm_v   = cJSON_GetObjectItem(obj, "time_mode");
    cJSON* sd_v   = cJSON_GetObjectItem(obj, "start_date");
    if (!cJSON_IsString(id_v) || !cJSON_IsString(name_v) ||
        !cJSON_IsString(kind_v) || !cJSON_IsString(tm_v) ||
        !cJSON_IsString(sd_v)) return -1;
    if (strlen(id_v->valuestring)   > CALENDAR_ID_MAX)   return -1;
    if (strlen(name_v->valuestring) > CALENDAR_NAME_MAX) return -1;
    snprintf(out->id,   sizeof out->id,   "%s", id_v->valuestring);
    snprintf(out->name, sizeof out->name, "%s", name_v->valuestring);

    if      (strcmp(kind_v->valuestring, "single_date") == 0) out->kind = CALENDAR_KIND_SINGLE_DATE;
    else if (strcmp(kind_v->valuestring, "date_range")  == 0) out->kind = CALENDAR_KIND_DATE_RANGE;
    else if (strcmp(kind_v->valuestring, "annual")      == 0) out->kind = CALENDAR_KIND_ANNUAL;
    else return -1;

    if      (strcmp(tm_v->valuestring, "all_day")  == 0) out->time_mode = CALENDAR_TIME_ALL_DAY;
    else if (strcmp(tm_v->valuestring, "specific") == 0) out->time_mode = CALENDAR_TIME_SPECIFIC;
    else return -1;

    if (out->time_mode == CALENDAR_TIME_SPECIFIC) {
        cJSON* tod = cJSON_GetObjectItem(obj, "time_of_day");
        if (!cJSON_IsString(tod)) return -1;
        int h, m, s;
        if (sscanf(tod->valuestring, "%2d:%2d:%2d", &h, &m, &s) != 3) return -1;
        if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) return -1;
        out->time_of_day_seconds = h * 3600 + m * 60 + s;
    }

    int y, mo, d;
    if (sscanf(sd_v->valuestring, "%4d-%2d-%2d", &y, &mo, &d) != 3) return -1;
    out->start_date.year = y; out->start_date.month = mo; out->start_date.day = d;

    if (out->kind == CALENDAR_KIND_DATE_RANGE) {
        cJSON* ed_v = cJSON_GetObjectItem(obj, "end_date");
        if (!cJSON_IsString(ed_v)) return -1;
        if (sscanf(ed_v->valuestring, "%4d-%2d-%2d", &y, &mo, &d) != 3) return -1;
        out->end_date.year = y; out->end_date.month = mo; out->end_date.day = d;
    }

    cJSON* notes = cJSON_GetObjectItem(obj, "notes");
    if (notes) {
        if (!cJSON_IsString(notes)) return -1;
        if (strlen(notes->valuestring) > CALENDAR_NOTES_MAX) return -1;
        snprintf(out->notes, sizeof out->notes, "%s", notes->valuestring);
    }
    return 0;
}

// CALENDAR_ERR_* codes share their numeric values with ANCHORS_ERR_*
// for the same conditions; reuse the mapping helper above (handles
// invalid_annual_date by way of the generic validation_error tag).
static void HTTP_Endpoint_Calendar(const ACAP_HTTP_Response response,
                                   const ACAP_HTTP_Request  request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method) {
        respond_json_error(response, 400, "bad_request", "missing method");
        return;
    }

    if (strcmp(method, "GET") == 0) {
        cJSON* body = build_calendar_response();
        ACAP_HTTP_Respond_JSON(response, body);
        cJSON_Delete(body);
        return;
    }

    if (strcmp(method, "POST") == 0) {
        if (!request->postData) {
            respond_json_error(response, 400, "bad_request", "missing body");
            return;
        }
        cJSON* in = cJSON_Parse(request->postData);
        if (!in) {
            respond_json_error(response, 400, "bad_request", "body is not valid JSON");
            return;
        }
        cJSON* mode_v = cJSON_GetObjectItem(in, "mode");
        if (!cJSON_IsString(mode_v)) {
            cJSON_Delete(in);
            respond_json_error(response, 400, "bad_request", "missing 'mode'");
            return;
        }
        if (strcmp(mode_v->valuestring, "upsert") == 0) {
            cJSON* e_obj = cJSON_GetObjectItem(in, "entry");
            calendar_entry_t e;
            if (!e_obj || calendar_from_wire(e_obj, &e) != 0) {
                cJSON_Delete(in);
                respond_json_error(response, 400, "validation_error",
                                   "invalid calendar entry object");
                return;
            }
            calendar_entry_t existing;
            int found = (calendar_get_by_id(e.id, &existing) == 0);
            int rc = found ? calendar_update(&e) : calendar_create(&e);
            cJSON_Delete(in);
            if (rc != CALENDAR_OK) {
                const char* tag; const char* msg;
                int code = map_anchors_err(rc, &tag, &msg);
                // The contract distinguishes Feb-29 annual specifically.
                if (rc == CALENDAR_ERR_INVALID && e.kind == CALENDAR_KIND_ANNUAL &&
                    e.start_date.month == 2 && e.start_date.day == 29) {
                    respond_json_error(response, 400, "invalid_annual_date",
                                       "annual entries cannot use Feb 29; use Feb 28 or Mar 1");
                    return;
                }
                respond_json_error(response, code, tag, msg);
                return;
            }
            cJSON* body = build_calendar_response();
            ACAP_HTTP_Respond_JSON(response, body);
            cJSON_Delete(body);
            return;
        }
        if (strcmp(mode_v->valuestring, "replace_all") == 0) {
            cJSON* arr = cJSON_GetObjectItem(in, "entries");
            if (!cJSON_IsArray(arr)) {
                cJSON_Delete(in);
                respond_json_error(response, 400, "bad_request",
                                   "missing 'entries' array");
                return;
            }
            int n = cJSON_GetArraySize(arr);
            if (n > (int)CALENDAR_OPERATOR_MAX) {
                cJSON_Delete(in);
                respond_json_error(response, 413, "operator_cap",
                                   "batch exceeds 64-entry cap");
                return;
            }
            calendar_entry_t batch[CALENDAR_OPERATOR_MAX];
            for (int i = 0; i < n; i++) {
                cJSON* el = cJSON_GetArrayItem(arr, i);
                if (calendar_from_wire(el, &batch[i]) != 0) {
                    cJSON_Delete(in);
                    char msg[64];
                    snprintf(msg, sizeof msg, "calendar entry %d malformed", i);
                    respond_json_error(response, 400, "validation_error", msg);
                    return;
                }
            }
            cJSON_Delete(in);
            int rc = calendar_replace_all(batch, (size_t)n);
            if (rc != CALENDAR_OK) {
                const char* tag; const char* msg;
                int code = map_anchors_err(rc, &tag, &msg);
                respond_json_error(response, code, tag, msg);
                return;
            }
            cJSON* body = build_calendar_response();
            ACAP_HTTP_Respond_JSON(response, body);
            cJSON_Delete(body);
            return;
        }
        cJSON_Delete(in);
        respond_json_error(response, 400, "bad_request", "unknown 'mode'");
        return;
    }

    if (strcmp(method, "DELETE") == 0) {
        const char* id = ACAP_HTTP_Request_Param(request, "id");
        if (!id) {
            respond_json_error(response, 400, "bad_request", "missing 'id' query param");
            return;
        }
        char id_buf[CALENDAR_ID_MAX + 1];
        snprintf(id_buf, sizeof id_buf, "%s", id);
        free((void*)id);
        int rc = calendar_delete(id_buf);
        if (rc != CALENDAR_OK) {
            const char* tag; const char* msg;
            int code = map_anchors_err(rc, &tag, &msg);
            respond_json_error(response, code, tag, msg);
            return;
        }
        cJSON* body = build_calendar_response();
        ACAP_HTTP_Respond_JSON(response, body);
        cJSON_Delete(body);
        return;
    }

    respond_json_error(response, 405, "method_not_allowed", "use GET, POST, or DELETE");
}

// ---- Events (per-row toggle) endpoint -----------------------------

static void HTTP_Endpoint_Events(const ACAP_HTTP_Response response,
                                 const ACAP_HTTP_Request  request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method || strcmp(method, "POST") != 0) {
        respond_json_error(response, 405, "method_not_allowed", "use POST");
        return;
    }
    if (!request->postData) {
        respond_json_error(response, 400, "bad_request", "missing body");
        return;
    }
    cJSON* in = cJSON_Parse(request->postData);
    if (!in) {
        respond_json_error(response, 400, "bad_request", "body is not valid JSON");
        return;
    }
    cJSON* id_v = cJSON_GetObjectItem(in, "id");
    cJSON* en_v = cJSON_GetObjectItem(in, "enabled");
    if (!cJSON_IsString(id_v) || !cJSON_IsBool(en_v)) {
        cJSON_Delete(in);
        respond_json_error(response, 400, "bad_request",
                           "body must include string 'id' and boolean 'enabled'");
        return;
    }
    char id_buf[ANCHORS_ID_MAX + 1];
    snprintf(id_buf, sizeof id_buf, "%s", id_v->valuestring);
    int enabled = cJSON_IsTrue(en_v) ? 1 : 0;
    cJSON_Delete(in);

    int rc = anchors_set_enabled(id_buf, enabled);
    if (rc != ANCHORS_OK) {
        const char* tag; const char* msg;
        int code = map_anchors_err(rc, &tag, &msg);
        respond_json_error(response, code, tag, msg);
        return;
    }

    // Trigger a recompute so the suppressed/un-suppressed state takes
    // effect on currently-armed timers (FR-11.7 / DL-18).
    timers_recompute_now(RECOMPUTE_TRIGGER_CONFIG_CHANGE);

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "id", id_buf);
    cJSON_AddBoolToObject(body, "enabled", enabled);
    ACAP_HTTP_Respond_JSON(response, body);
    cJSON_Delete(body);
}

// ---- events_today (Schedule-list preview) -------------------------

// Resolve "next fire" for a built-in anchor row over a 1-day window.
static void resolve_built_in_next_fire(const anchor_t* a, time_t now,
                                       time_t* out, time_t* end_out) {
    *out = (time_t)-1;
    if (end_out) *end_out = (time_t)0;
    time_t resolved = 0;
    if (anchors_resolve_source(a->id, now, &resolved) != 0) return;
    if (resolved == SOLAR_NO_EVENT || resolved == LUNAR_NO_EVENT) return;
    if (resolved <= now) {
        // Already passed today; for daily built-ins re-resolve for
        // tomorrow as a best-effort. Phase / season events resolve as
        // "next-after-now" already.
        time_t tomorrow = now + 86400;
        if (anchors_resolve_source(a->id, tomorrow, &resolved) != 0) return;
        if (resolved == SOLAR_NO_EVENT || resolved == LUNAR_NO_EVENT) return;
    }
    *out = resolved;
}

static const char* built_in_category(const char* id) {
    static const char* solar_ids[] = {
        "sunrise", "sunset", "sunnoon", "sunmidnight",
        "civildawn", "civildusk", "nauticaldawn", "nauticaldusk",
        "astrodawn", "astrodusk", NULL
    };
    static const char* lunar_ids[] = {
        "moonrise", "moonset", "moonnoon", "moonmidnight",
        "newmoon", "firstquarter", "fullmoon", "lastquarter", NULL
    };
    static const char* season_ids[] = {
        "marchequinox", "junesolstice", "septemberequinox",
        "decembersolstice", NULL
    };
    for (int i = 0; solar_ids[i];  i++) if (strcmp(id, solar_ids[i])  == 0) return "solar";
    for (int i = 0; lunar_ids[i];  i++) if (strcmp(id, lunar_ids[i])  == 0) return "lunar";
    for (int i = 0; season_ids[i]; i++) if (strcmp(id, season_ids[i]) == 0) return "seasonal";
    return "anchor";
}

// Build one events_today row for an anchor (built-in or operator).
static cJSON* build_events_row_anchor(const anchor_t* a, time_t now) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id",   a->id);
    cJSON_AddStringToObject(o, "name", a->name);
    cJSON_AddStringToObject(o, "category",
        a->built_in ? built_in_category(a->id) : "anchor");
    const char* kind_s =
        (a->kind == ANCHOR_KIND_OFFSET)    ? "offset" :
        (a->kind == ANCHOR_KIND_PAIRED)    ? "paired" :
        (a->kind == ANCHOR_KIND_THRESHOLD) ? "threshold" : "offset";
    cJSON_AddStringToObject(o, "kind", kind_s);
    char topic[200];
    snprintf(topic, sizeof topic,
             "tnsaxis:CameraApplicationPlatform/" APP_PACKAGE "/%s", a->id);
    cJSON_AddStringToObject(o, "topic", topic);
    // Always consult the toggle store; the in-memory anchor's `enabled`
    // is kept in sync at set_enabled time but for symmetry with the
    // calendar path we use the same source-of-truth.
    int enabled_now = anchors_is_enabled(a->id);
    cJSON_AddBoolToObject(o, "enabled", enabled_now);

    int stateful = (a->kind == ANCHOR_KIND_PAIRED) ||
                   (a->kind == ANCHOR_KIND_OFFSET && a->duration_minutes > 0);
    cJSON_AddBoolToObject(o, "stateful", stateful);

    if (!enabled_now) {
        cJSON_AddNullToObject(o, "next_fire_utc");
        cJSON_AddNullToObject(o, "next_fire_local");
        if (stateful) {
            cJSON_AddNullToObject(o, "next_end_utc");
            cJSON_AddNullToObject(o, "next_end_local");
        }
        cJSON_AddBoolToObject(o, "not_firing_today", 1);
        cJSON_AddStringToObject(o, "not_firing_reason", "disabled");
        return o;
    }

    time_t fire = (time_t)-1, end = (time_t)0;
    if (a->built_in) {
        resolve_built_in_next_fire(a, now, &fire, &end);
    } else {
        // Operator anchors: compute a single next-fire across kinds.
        switch (a->kind) {
            case ANCHOR_KIND_OFFSET: {
                time_t base = 0;
                if (anchors_resolve_source(a->event_source, now, &base) == 0 &&
                    base != SOLAR_NO_EVENT) {
                    time_t when = base + (time_t)(a->offset_minutes * 60);
                    if (when > now) {
                        fire = when;
                        if (a->duration_minutes > 0)
                            end = when + (time_t)(a->duration_minutes * 60);
                    }
                }
                break;
            }
            case ANCHOR_KIND_PAIRED: {
                time_t s = 0, e = 0;
                if (anchors_resolve_source(a->start_event, now, &s) == 0 &&
                    anchors_resolve_source(a->end_event,   now, &e) == 0 &&
                    s != SOLAR_NO_EVENT && e != SOLAR_NO_EVENT) {
                    time_t s_when = s + (time_t)(a->start_offset_minutes * 60);
                    time_t e_when = e + (time_t)(a->end_offset_minutes * 60);
                    if (e_when <= s_when) e_when += 86400;
                    if (s_when > now) { fire = s_when; end = e_when; }
                }
                break;
            }
            case ANCHOR_KIND_THRESHOLD:
                // Cheap proxy: today only. Same as the timer arm.
                fire = (time_t)-1;
                break;
        }
    }

    if (fire <= 0) {
        cJSON_AddNullToObject(o, "next_fire_utc");
        cJSON_AddNullToObject(o, "next_fire_local");
        if (stateful) {
            cJSON_AddNullToObject(o, "next_end_utc");
            cJSON_AddNullToObject(o, "next_end_local");
        }
        cJSON_AddBoolToObject(o, "not_firing_today", 1);
        // Best-effort reason; M6 emits a small set documented in the
        // contract. "out_of_range" covers the catch-all.
        const char* category = a->built_in ? built_in_category(a->id) : "anchor";
        const char* reason = "out_of_range";
        if (a->kind == ANCHOR_KIND_THRESHOLD) reason = "threshold_unmet";
        else if (strcmp(category, "lunar") == 0) reason = "lunar_no_event";
        else if (strcmp(category, "solar") == 0) reason = "solar_no_event";
        cJSON_AddStringToObject(o, "not_firing_reason", reason);
        return o;
    }
    char utc_buf[40], loc_buf[40];
    format_iso_utc(fire, utc_buf, sizeof utc_buf);
    format_iso_local(fire, loc_buf, sizeof loc_buf);
    cJSON_AddStringToObject(o, "next_fire_utc",   utc_buf);
    cJSON_AddStringToObject(o, "next_fire_local", loc_buf);
    if (stateful) {
        if (end > 0) {
            format_iso_utc(end, utc_buf, sizeof utc_buf);
            format_iso_local(end, loc_buf, sizeof loc_buf);
            cJSON_AddStringToObject(o, "next_end_utc",   utc_buf);
            cJSON_AddStringToObject(o, "next_end_local", loc_buf);
        } else {
            cJSON_AddNullToObject(o, "next_end_utc");
            cJSON_AddNullToObject(o, "next_end_local");
        }
    }
    return o;
}

static cJSON* build_events_row_calendar(const calendar_entry_t* e, time_t now) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id",   e->id);
    cJSON_AddStringToObject(o, "name", e->name);
    cJSON_AddStringToObject(o, "category", "calendar");
    const char* kind_s =
        (e->kind == CALENDAR_KIND_SINGLE_DATE) ? "single_date" :
        (e->kind == CALENDAR_KIND_DATE_RANGE)  ? "date_range"  :
        "annual";
    cJSON_AddStringToObject(o, "kind", kind_s);
    char topic[200];
    snprintf(topic, sizeof topic,
             "tnsaxis:CameraApplicationPlatform/" APP_PACKAGE "/%s", e->id);
    cJSON_AddStringToObject(o, "topic", topic);
    int enabled_now = anchors_is_enabled(e->id);  // authoritative
    cJSON_AddBoolToObject(o, "enabled", enabled_now);
    int stateful = (e->time_mode == CALENDAR_TIME_ALL_DAY);
    cJSON_AddBoolToObject(o, "stateful", stateful);

    if (!enabled_now) {
        cJSON_AddNullToObject(o, "next_fire_utc");
        cJSON_AddNullToObject(o, "next_fire_local");
        if (stateful) {
            cJSON_AddNullToObject(o, "next_end_utc");
            cJSON_AddNullToObject(o, "next_end_local");
        }
        cJSON_AddBoolToObject(o, "not_firing_today", 1);
        cJSON_AddStringToObject(o, "not_firing_reason", "disabled");
        return o;
    }

    time_t fire = (time_t)-1, end = (time_t)0;
    (void)calendar_next_occurrence(e->id, now, &fire, &end);
    if (fire == (time_t)-1) {
        cJSON_AddNullToObject(o, "next_fire_utc");
        cJSON_AddNullToObject(o, "next_fire_local");
        if (stateful) {
            cJSON_AddNullToObject(o, "next_end_utc");
            cJSON_AddNullToObject(o, "next_end_local");
        }
        cJSON_AddBoolToObject(o, "not_firing_today", 1);
        cJSON_AddStringToObject(o, "not_firing_reason", "out_of_range");
        return o;
    }
    char utc_buf[40], loc_buf[40];
    format_iso_utc(fire, utc_buf, sizeof utc_buf);
    format_iso_local(fire, loc_buf, sizeof loc_buf);
    cJSON_AddStringToObject(o, "next_fire_utc",   utc_buf);
    cJSON_AddStringToObject(o, "next_fire_local", loc_buf);
    if (stateful && end > 0) {
        format_iso_utc(end, utc_buf, sizeof utc_buf);
        format_iso_local(end, loc_buf, sizeof loc_buf);
        cJSON_AddStringToObject(o, "next_end_utc",   utc_buf);
        cJSON_AddStringToObject(o, "next_end_local", loc_buf);
    } else if (stateful) {
        cJSON_AddNullToObject(o, "next_end_utc");
        cJSON_AddNullToObject(o, "next_end_local");
    }
    return o;
}

static void HTTP_Endpoint_Events_Today(const ACAP_HTTP_Response response,
                                       const ACAP_HTTP_Request  request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method || strcmp(method, "GET") != 0) {
        respond_json_error(response, 405, "method_not_allowed", "use GET");
        return;
    }
    // Default to AXParameter LookaheadDays (FR-12.2); ?lookahead_days=N
    // overrides per-request.
    int lookahead_days = g_param_lookahead_days;
    const char* la = ACAP_HTTP_Request_Param(request, "lookahead_days");
    if (la) {
        int v = atoi(la);
        if (v >= 1 && v <= 365) lookahead_days = v;
        free((void*)la);
    }

    time_t now = time(NULL);

    cJSON* root = cJSON_CreateObject();
    char utc_buf[40];
    format_iso_utc(now, utc_buf, sizeof utc_buf);
    cJSON_AddStringToObject(root, "computed_at_utc", utc_buf);
    cJSON_AddNumberToObject(root, "lookahead_days",  lookahead_days);
    cJSON_AddNumberToObject(root, "lat", ACAP_DEVICE_Latitude());
    cJSON_AddNumberToObject(root, "lon", ACAP_DEVICE_Longitude());
    cJSON* rows = cJSON_AddArrayToObject(root, "rows");

    size_t na = anchors_count();
    for (size_t i = 0; i < na; i++) {
        anchor_t a;
        if (anchors_get_by_index(i, &a) != 0) continue;
        cJSON_AddItemToArray(rows, build_events_row_anchor(&a, now));
    }
    size_t nc = calendar_count();
    for (size_t i = 0; i < nc; i++) {
        calendar_entry_t e;
        if (calendar_get_by_index(i, &e) != 0) continue;
        cJSON_AddItemToArray(rows, build_events_row_calendar(&e, now));
    }

    ACAP_HTTP_Respond_JSON(response, root);
    cJSON_Delete(root);
}

// ---- M7: status / recompute / export / import / debug -------------

// Add a recompute_summary_t to a parent JSON object as `key` (or to
// `parent` directly when key is NULL).
static cJSON* summary_to_json(const recompute_summary_t* s) {
    char buf[40];
    cJSON* o = cJSON_CreateObject();
    format_iso_utc(s->started_at_utc, buf, sizeof buf);
    cJSON_AddStringToObject(o, "started_at", buf);
    cJSON_AddStringToObject(o, "trigger", status_trigger_str(s->trigger));
    cJSON_AddNumberToObject(o, "elapsed_ms",        s->elapsed_ms);
    cJSON_AddNumberToObject(o, "anchors_evaluated", s->anchors_evaluated);
    cJSON_AddNumberToObject(o, "events_armed",      s->events_armed);
    cJSON_AddNumberToObject(o, "skipped_polar",     s->skipped_polar);
    cJSON_AddNumberToObject(o, "skipped_disabled",  s->skipped_disabled);
    cJSON_AddNumberToObject(o, "skipped_past",      s->skipped_past);
    cJSON_AddNumberToObject(o, "errors",            s->errors);
    return o;
}

// Best-effort timezone read. Returns the IANA name from /etc/timezone
// (the camera's standard location) or "" if unreadable. The offset is
// derived from struct tm::tm_gmtoff for `now`.
static void describe_tz(time_t now, char* tz_name, size_t tz_len, int* offset_seconds) {
    if (tz_name && tz_len) tz_name[0] = '\0';
    if (offset_seconds) *offset_seconds = 0;
    FILE* f = fopen("/etc/timezone", "r");
    if (f) {
        if (fgets(tz_name, (int)tz_len, f)) {
            // strip trailing newline
            size_t n = strlen(tz_name);
            while (n > 0 && (tz_name[n-1] == '\n' || tz_name[n-1] == '\r')) {
                tz_name[--n] = '\0';
            }
        }
        fclose(f);
    }
    struct tm tm;
    localtime_r(&now, &tm);
    if (offset_seconds) *offset_seconds = (int)tm.tm_gmtoff;
}

// Build the AXParameter scalar mirror as JSON (used by /status and /debug).
static cJSON* build_axparams_json(void) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "lookahead_days",       g_param_lookahead_days);
    cJSON_AddStringToObject(o, "event_name_prefix",    g_param_event_name_prefix);
    cJSON_AddNumberToObject(o, "poll_interval_seconds", g_param_poll_interval_seconds);
    return o;
}

static void HTTP_Endpoint_Status(const ACAP_HTTP_Response response,
                                 const ACAP_HTTP_Request  request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method || strcmp(method, "GET") != 0) {
        respond_json_error(response, 405, "method_not_allowed", "use GET");
        return;
    }

    time_t now = time(NULL);
    char utc_buf[40], local_buf[40];
    format_iso_utc(now, utc_buf, sizeof utc_buf);
    format_iso_local(now, local_buf, sizeof local_buf);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", APP_VERSION);
    cJSON_AddStringToObject(root, "now",       utc_buf);
    cJSON_AddStringToObject(root, "now_local", local_buf);

    // Location block.
    cJSON* loc = cJSON_AddObjectToObject(root, "location");
    double lat = ACAP_DEVICE_Latitude();
    double lon = ACAP_DEVICE_Longitude();
    int loc_valid = (lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0);
    cJSON_AddNumberToObject(loc, "lat", lat);
    cJSON_AddNumberToObject(loc, "lon", lon);
    cJSON_AddBoolToObject(loc, "valid", loc_valid);
    char tz_name[64];
    int tz_off = 0;
    describe_tz(now, tz_name, sizeof tz_name, &tz_off);
    cJSON_AddStringToObject(loc, "tz", tz_name);
    cJSON_AddNumberToObject(loc, "tz_offset_seconds", tz_off);

    // Counts. topics_declared == anchors + calendar entries (each one
    // owns a topic via ACAP_EVENTS_Add_Event); topics_enabled walks the
    // FR-11.7 gate.
    int topics_declared = 0, topics_enabled = 0, anchors_user = 0;
    size_t na = anchors_count();
    for (size_t i = 0; i < na; i++) {
        anchor_t a;
        if (anchors_get_by_index(i, &a) != 0) continue;
        topics_declared++;
        if (anchors_is_enabled(a.id)) topics_enabled++;
        if (!a.built_in) anchors_user++;
    }
    int calendar_entries = (int)calendar_count();
    for (int i = 0; i < calendar_entries; i++) {
        calendar_entry_t e;
        if (calendar_get_by_index((size_t)i, &e) != 0) continue;
        topics_declared++;
        if (anchors_is_enabled(e.id)) topics_enabled++;
    }
    cJSON* counts = cJSON_AddObjectToObject(root, "counts");
    cJSON_AddNumberToObject(counts, "topics_declared",  topics_declared);
    cJSON_AddNumberToObject(counts, "topics_enabled",   topics_enabled);
    cJSON_AddNumberToObject(counts, "anchors_user",     anchors_user);
    cJSON_AddNumberToObject(counts, "calendar_entries", calendar_entries);

    // last_recompute.
    const recompute_summary_t* last = status_last();
    if (last) {
        cJSON* o = summary_to_json(last);
        cJSON_AddItemToObject(root, "last_recompute", o);
    } else {
        cJSON_AddNullToObject(root, "last_recompute");
    }

    // next_recompute.
    time_t next_utc = 0;
    recompute_trigger_t next_reason = RECOMPUTE_TRIGGER_MIDNIGHT;
    status_get_next(&next_utc, &next_reason);
    cJSON* nxt = cJSON_AddObjectToObject(root, "next_recompute");
    if (next_utc > 0) {
        char nb[40];
        format_iso_utc(next_utc, nb, sizeof nb);
        cJSON_AddStringToObject(nxt, "scheduled_at", nb);
    } else {
        cJSON_AddNullToObject(nxt, "scheduled_at");
    }
    cJSON_AddStringToObject(nxt, "reason", status_trigger_str(next_reason));

    // recent[]. status_recent() returns a snapshot; iterate and serialize.
    int recent_count = 0;
    const recompute_summary_t* recent = status_recent(&recent_count);
    cJSON* arr = cJSON_AddArrayToObject(root, "recent");
    for (int i = 0; i < recent_count; i++) {
        cJSON_AddItemToArray(arr, summary_to_json(&recent[i]));
    }

    cJSON_AddBoolToObject(root, "debug_logging", g_debug_logging_enabled);
    cJSON_AddItemToObject(root, "axparameters", build_axparams_json());

    // rss_kb (DL-21): VmRSS from /proc/self/status. 0 if unreadable
    // (sandboxed /proc, ENOENT, etc.) — non-fatal for the soak harness.
    long rss_kb = 0;
    FILE* f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "VmRSS:", 6) == 0) {
                rss_kb = strtol(line + 6, NULL, 10);
                break;
            }
        }
        fclose(f);
    }
    cJSON_AddNumberToObject(root, "rss_kb", (double)rss_kb);

    ACAP_HTTP_Respond_JSON(response, root);
    cJSON_Delete(root);
}

// Manual-recompute endpoint (FR-10.2). Always uses RECOMPUTE_TRIGGER_MANUAL.
// Returns 202 + {"queued":true} when coalesced; otherwise the new
// last_recompute summary at HTTP 200.
static void HTTP_Endpoint_Recompute(const ACAP_HTTP_Response response,
                                    const ACAP_HTTP_Request  request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method || strcmp(method, "POST") != 0) {
        respond_json_error(response, 405, "method_not_allowed", "use POST");
        return;
    }
    int rc = timers_recompute_now(RECOMPUTE_TRIGGER_MANUAL);
    if (rc == TIMERS_RECOMPUTE_QUEUED) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "queued", 1);
        char* body = cJSON_PrintUnformatted(o);
        if (body) {
            ACAP_HTTP_Respond_String(response,
                "Status: 202 Accepted\r\n"
                "Content-Type: application/json\r\n"
                "\r\n"
                "%s", body);
            free(body);
        }
        cJSON_Delete(o);
        return;
    }
    if (rc == TIMERS_RECOMPUTE_ERROR) {
        respond_json_error(response, 500, "recompute_failed",
                           "internal error during recompute");
        return;
    }
    const recompute_summary_t* last = status_last();
    if (!last) {
        respond_json_error(response, 500, "no_status",
                           "recompute completed but no summary recorded");
        return;
    }
    cJSON* o = summary_to_json(last);
    ACAP_HTTP_Respond_JSON(response, o);
    cJSON_Delete(o);
}

// Build the export envelope payload (also used by /export to serialize
// to a temp string for Content-Length / streaming). Caller frees.
static cJSON* build_export_envelope(void) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "schema",  "camera-schedule.config.v1");
    cJSON_AddStringToObject(root, "version", APP_VERSION);
    char ts[40];
    format_iso_utc(time(NULL), ts, sizeof ts);
    cJSON_AddStringToObject(root, "exported_at", ts);

    cJSON_AddItemToObject(root, "axparameters", build_axparams_json());

    // anchors[]: operator-defined only. Built-ins are derived from
    // settings/events.json on every boot and MUST NOT be exported.
    cJSON* anchors_arr = cJSON_AddArrayToObject(root, "anchors");
    size_t na = anchors_count();
    for (size_t i = 0; i < na; i++) {
        anchor_t a;
        if (anchors_get_by_index(i, &a) != 0) continue;
        if (a.built_in) continue;
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id",   a.id);
        cJSON_AddStringToObject(o, "name", a.name);
        const char* kind_s =
            (a.kind == ANCHOR_KIND_OFFSET)    ? "offset" :
            (a.kind == ANCHOR_KIND_PAIRED)    ? "paired" :
            (a.kind == ANCHOR_KIND_THRESHOLD) ? "threshold" : "offset";
        cJSON_AddStringToObject(o, "kind", kind_s);
        switch (a.kind) {
            case ANCHOR_KIND_OFFSET:
                cJSON_AddStringToObject(o, "event_source",     a.event_source);
                cJSON_AddNumberToObject(o, "offset_minutes",   a.offset_minutes);
                cJSON_AddNumberToObject(o, "duration_minutes", a.duration_minutes);
                break;
            case ANCHOR_KIND_PAIRED:
                cJSON_AddStringToObject(o, "start_event",          a.start_event);
                cJSON_AddNumberToObject(o, "start_offset_minutes", a.start_offset_minutes);
                cJSON_AddStringToObject(o, "end_event",            a.end_event);
                cJSON_AddNumberToObject(o, "end_offset_minutes",   a.end_offset_minutes);
                break;
            case ANCHOR_KIND_THRESHOLD: {
                cJSON_AddStringToObject(o, "metric", "moon_illumination");
                const char* op_s =
                    (a.op == ANCHOR_OP_GE) ? "ge" :
                    (a.op == ANCHOR_OP_LE) ? "le" :
                    (a.op == ANCHOR_OP_GT) ? "gt" : "lt";
                cJSON_AddStringToObject(o, "op", op_s);
                cJSON_AddNumberToObject(o, "value", a.value);
                break;
            }
        }
        cJSON_AddItemToArray(anchors_arr, o);
    }

    // calendar[]: full set.
    cJSON* cal_arr = cJSON_AddArrayToObject(root, "calendar");
    size_t nc = calendar_count();
    for (size_t i = 0; i < nc; i++) {
        calendar_entry_t e;
        if (calendar_get_by_index(i, &e) != 0) continue;
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id",   e.id);
        cJSON_AddStringToObject(o, "name", e.name);
        const char* kind_s =
            (e.kind == CALENDAR_KIND_SINGLE_DATE) ? "single_date" :
            (e.kind == CALENDAR_KIND_DATE_RANGE)  ? "date_range"  : "annual";
        cJSON_AddStringToObject(o, "kind", kind_s);
        cJSON_AddStringToObject(o, "time_mode",
            e.time_mode == CALENDAR_TIME_ALL_DAY ? "all_day" : "specific");
        if (e.time_mode == CALENDAR_TIME_SPECIFIC) {
            char buf[16];
            int s = e.time_of_day_seconds;
            snprintf(buf, sizeof buf, "%02d:%02d:%02d",
                     s / 3600, (s % 3600) / 60, s % 60);
            cJSON_AddStringToObject(o, "time_of_day", buf);
        }
        char date_buf[16];
        if (e.kind == CALENDAR_KIND_ANNUAL) {
            snprintf(date_buf, sizeof date_buf, "2000-%02d-%02d",
                     e.start_date.month, e.start_date.day);
        } else {
            snprintf(date_buf, sizeof date_buf, "%04d-%02d-%02d",
                     e.start_date.year, e.start_date.month, e.start_date.day);
        }
        cJSON_AddStringToObject(o, "start_date", date_buf);
        if (e.kind == CALENDAR_KIND_DATE_RANGE) {
            snprintf(date_buf, sizeof date_buf, "%04d-%02d-%02d",
                     e.end_date.year, e.end_date.month, e.end_date.day);
            cJSON_AddStringToObject(o, "end_date", date_buf);
        }
        cJSON_AddStringToObject(o, "notes", e.notes);
        cJSON_AddItemToArray(cal_arr, o);
    }

    // schedule_enabled: walk every known id and emit only the
    // explicitly-disabled ones (default-on per FR-11.7). The export
    // round-trip retains correctness: missing-from-import keys re-default
    // to enabled on the importing camera.
    cJSON* en = cJSON_AddObjectToObject(root, "schedule_enabled");
    for (size_t i = 0; i < na; i++) {
        anchor_t a;
        if (anchors_get_by_index(i, &a) != 0) continue;
        if (!anchors_is_enabled(a.id))
            cJSON_AddBoolToObject(en, a.id, 0);
    }
    for (size_t i = 0; i < nc; i++) {
        calendar_entry_t e;
        if (calendar_get_by_index(i, &e) != 0) continue;
        if (!anchors_is_enabled(e.id))
            cJSON_AddBoolToObject(en, e.id, 0);
    }

    cJSON_AddBoolToObject(root, "debug_logging", g_debug_logging_enabled);
    return root;
}

static void HTTP_Endpoint_Export(const ACAP_HTTP_Response response,
                                 const ACAP_HTTP_Request  request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method || strcmp(method, "GET") != 0) {
        respond_json_error(response, 405, "method_not_allowed", "use GET");
        return;
    }
    cJSON* env = build_export_envelope();
    char* body = cJSON_Print(env);
    cJSON_Delete(env);
    if (!body) {
        respond_json_error(response, 500, "serialize_failed",
                           "could not serialize export envelope");
        return;
    }

    // Best-effort filename: camera_schedule_<host>_<YYYYMMDD>.json
    char host[64] = {0};
    if (gethostname(host, sizeof host - 1) != 0 || host[0] == '\0') {
        snprintf(host, sizeof host, "host");
    }
    char datebuf[16];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(datebuf, sizeof datebuf, "%Y%m%d", &tm);
    char filename[160];
    snprintf(filename, sizeof filename,
             "camera_schedule_%s_%s.json", host, datebuf);

    // Custom header set: Content-Type + Content-Disposition. Use
    // Respond_String for the header (small) and Respond_Data for the
    // body to dodge the 4096-byte cap.
    ACAP_HTTP_Respond_String(response,
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n", filename);
    size_t blen = strlen(body);
    ACAP_HTTP_Respond_Data(response, blen, body);
    free(body);
}

// Validate that a string matches ^[a-z0-9_]{1,32}$ — same regex anchors
// + calendar enforce. Used by the schedule_enabled validator.
static int is_valid_id_str(const char* s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (n < 1 || n > 32) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

// The anchor_from_wire / calendar_from_wire helpers are defined as
// static earlier in this translation unit (lines 303 and 550 in this
// file). They handle the same wire shape that /anchors and /calendar
// accept, so the import path can reuse them directly.
static void HTTP_Endpoint_Import(const ACAP_HTTP_Response response,
                                 const ACAP_HTTP_Request  request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method || strcmp(method, "POST") != 0) {
        respond_json_error(response, 405, "method_not_allowed", "use POST");
        return;
    }
    if (!request->postData) {
        respond_json_error(response, 400, "malformed_json", "missing body");
        return;
    }
    cJSON* in = cJSON_Parse(request->postData);
    if (!in) {
        respond_json_error(response, 400, "malformed_json",
                           "request body is not valid JSON");
        return;
    }

    // Step 1: schema field.
    cJSON* schema_v = cJSON_GetObjectItem(in, "schema");
    if (!cJSON_IsString(schema_v) ||
        strcmp(schema_v->valuestring, "camera-schedule.config.v1") != 0) {
        cJSON_Delete(in);
        respond_json_error(response, 400, "schema_mismatch",
                           "expected camera-schedule.config.v1");
        return;
    }

    // Step 2: parse anchors[] into an in-memory batch and validate.
    cJSON* anchors_arr = cJSON_GetObjectItem(in, "anchors");
    int n_anchors = (anchors_arr && cJSON_IsArray(anchors_arr))
                        ? cJSON_GetArraySize(anchors_arr) : 0;
    if (n_anchors > (int)ANCHORS_OPERATOR_MAX) {
        cJSON_Delete(in);
        respond_json_error(response, 400, "operator_cap",
                           "anchors count exceeds 64");
        return;
    }
    anchor_t anchor_batch[ANCHORS_OPERATOR_MAX];
    for (int i = 0; i < n_anchors; i++) {
        cJSON* el = cJSON_GetArrayItem(anchors_arr, i);
        if (anchor_from_wire(el, &anchor_batch[i]) != 0) {
            cJSON_Delete(in);
            char msg[80];
            snprintf(msg, sizeof msg, "anchor %d malformed", i);
            respond_json_error(response, 400, "invalid_anchor", msg);
            return;
        }
    }

    // Step 3: parse calendar[].
    cJSON* cal_arr = cJSON_GetObjectItem(in, "calendar");
    int n_cal = (cal_arr && cJSON_IsArray(cal_arr))
                    ? cJSON_GetArraySize(cal_arr) : 0;
    if (n_cal > (int)CALENDAR_OPERATOR_MAX) {
        cJSON_Delete(in);
        respond_json_error(response, 400, "operator_cap",
                           "calendar count exceeds 64");
        return;
    }
    calendar_entry_t cal_batch[CALENDAR_OPERATOR_MAX];
    for (int i = 0; i < n_cal; i++) {
        cJSON* el = cJSON_GetArrayItem(cal_arr, i);
        if (calendar_from_wire(el, &cal_batch[i]) != 0) {
            cJSON_Delete(in);
            char msg[80];
            snprintf(msg, sizeof msg, "calendar entry %d malformed", i);
            respond_json_error(response, 400, "invalid_calendar", msg);
            return;
        }
    }

    // Step 4: validate schedule_enabled keys + values.
    cJSON* en_obj = cJSON_GetObjectItem(in, "schedule_enabled");
    int n_en = 0;
    if (en_obj) {
        if (!cJSON_IsObject(en_obj)) {
            cJSON_Delete(in);
            respond_json_error(response, 400, "invalid_schedule_enabled",
                               "schedule_enabled must be an object");
            return;
        }
        cJSON* child = en_obj->child;
        while (child) {
            if (!is_valid_id_str(child->string) || !cJSON_IsBool(child)) {
                cJSON_Delete(in);
                char msg[96];
                snprintf(msg, sizeof msg,
                         "schedule_enabled key '%s' invalid",
                         child->string ? child->string : "(null)");
                respond_json_error(response, 400, "invalid_schedule_enabled", msg);
                return;
            }
            n_en++;
            child = child->next;
        }
    }

    cJSON* dbg_v = cJSON_GetObjectItem(in, "debug_logging");
    int new_debug = g_debug_logging_enabled;
    if (dbg_v) {
        if (!cJSON_IsBool(dbg_v)) {
            cJSON_Delete(in);
            respond_json_error(response, 400, "invalid_debug_logging",
                               "debug_logging must be a boolean");
            return;
        }
        new_debug = cJSON_IsTrue(dbg_v) ? 1 : 0;
    }

    // Cross-batch pre-flight: an anchor and a calendar entry sharing
    // the same id would survive the per-batch shape validation but
    // make calendar_replace_all reject (CALENDAR_ERR_DUPLICATE) AFTER
    // anchors_replace_all has already mutated state. Catch it here
    // before any disk writes so we never leave the on-disk + in-memory
    // halves inconsistent.
    for (int i = 0; i < n_anchors; i++) {
        for (int j = 0; j < n_cal; j++) {
            if (strcmp(anchor_batch[i].id, cal_batch[j].id) == 0) {
                cJSON_Delete(in);
                char msg[96];
                snprintf(msg, sizeof msg,
                         "id '%s' appears in both anchors[] and calendar[]",
                         anchor_batch[i].id);
                respond_json_error(response, 400, "id_conflict", msg);
                return;
            }
        }
    }

    // Pre-import safety: rename live config files aside as
    // *.before-import-<ts> so we have a rollback path if a later step
    // fails. Per contract §1.4 step 6: if the apply phase fails part-
    // way, the prior config MUST be preserved unchanged. We track
    // which renames succeeded and reverse them on failure.
    //
    // anchor_from_wire / calendar_from_wire validate parse shape only.
    // anchors_replace_all / calendar_replace_all do cross-namespace
    // dependency, regex, and built-in-collision checks that can reject
    // batches that survived shape validation — so the failure mode is
    // reachable, not theoretical.
    //
    // Caveat: this rolls back the on-disk file but cannot roll back
    // the anchors_replace_all in-memory state. If anchors apply
    // succeeds and calendar apply fails, the running app has new
    // anchors in memory until the next boot or a clean import. This
    // matches the contract's "prior config preserved" guarantee
    // (it's about persistent state) and any subsequent boot
    // reconciles. A pre-flight validator that mirrors
    // anchors_replace_all's checks would close this window; the
    // apparent gain doesn't justify exposing internal validators in
    // anchors.h, so we leave it as a follow-up.
    long long ts = (long long)time(NULL);
    const char* sandbox = ACAP_FILE_AppPath();
    const char* targets[] = {
        "localdata/anchors.json",
        "localdata/calendar.json",
        "localdata/schedule_enabled.json",
        NULL
    };
    char saved_src[3][256];
    char saved_dst[3][300];
    int  saved_n = 0;
    if (sandbox) {
        for (int i = 0; targets[i]; i++) {
            snprintf(saved_src[i], sizeof saved_src[i], "%s%s",
                     sandbox, targets[i]);
            snprintf(saved_dst[i], sizeof saved_dst[i],
                     "%s%s.before-import-%lld",
                     sandbox, targets[i], ts);
            if (rename(saved_src[i], saved_dst[i]) == 0) {
                LOG("import: archived %s -> %s.before-import-%lld",
                    targets[i], targets[i], ts);
                saved_n++;
            } else if (errno != ENOENT) {
                LOG_WARN("import: copy-aside of %s failed: %s",
                         targets[i], strerror(errno));
            } else {
                // ENOENT — no live file to archive. Leave saved_n at
                // its current value; the rollback walk below will skip
                // entries whose dst doesn't exist.
            }
        }
    }

    // Restore-on-failure helper, used by every error path between here
    // and the success response. Walks `targets` in reverse so the most
    // recent rename is undone first, and unlinks any new file produced
    // by a partial apply step before swinging the saved-aside copy
    // back into place.
#define IMPORT_ROLLBACK_AND_FAIL(tag_str, msg_str) do { \
    for (int _i = 0; targets[_i]; _i++) { \
        /* Best-effort: clobber any partial new file the apply step left. */ \
        unlink(saved_src[_i]); \
        if (rename(saved_dst[_i], saved_src[_i]) == 0) { \
            LOG("import rollback: restored %s", targets[_i]); \
        } else if (errno != ENOENT) { \
            LOG_WARN("import rollback: rename(%s) failed: %s", \
                     targets[_i], strerror(errno)); \
        } \
    } \
    cJSON_Delete(in); \
    respond_json_error(response, 500, (tag_str), (msg_str)); \
    return; \
} while (0)

    (void)saved_n;  // silence -Wunused if rollback macro unused on a path

    // Step 5: apply. All-or-nothing semantics enforced by the rollback
    // macro above — any failure beyond this point swings the
    // *.before-import-<ts> copies back into place.
    int rc = anchors_replace_all(anchor_batch, (size_t)n_anchors);
    if (rc != ANCHORS_OK) {
        const char* tag; const char* msg;
        (void)map_anchors_err(rc, &tag, &msg);
        IMPORT_ROLLBACK_AND_FAIL(tag, msg);
    }
    rc = calendar_replace_all(cal_batch, (size_t)n_cal);
    if (rc != CALENDAR_OK) {
        const char* tag; const char* msg;
        (void)map_anchors_err(rc, &tag, &msg);
        IMPORT_ROLLBACK_AND_FAIL(tag, msg);
    }
    if (en_obj) {
        cJSON* child = en_obj->child;
        int enabled_rc = ANCHORS_OK;
        while (child) {
            int en = cJSON_IsTrue(child) ? 1 : 0;
            int r = anchors_set_enabled(child->string, en);
            // ANCHORS_ERR_NOT_FOUND is benign here — an exported
            // schedule_enabled key for an id that doesn't exist on
            // this camera (e.g. operator-renamed an anchor) is the
            // forward-compat path FR-12.4 expects to silently absorb.
            if (r != ANCHORS_OK && r != ANCHORS_ERR_NOT_FOUND) {
                enabled_rc = r;
                break;
            }
            child = child->next;
        }
        if (enabled_rc != ANCHORS_OK) {
            const char* tag; const char* msg;
            (void)map_anchors_err(enabled_rc, &tag, &msg);
            IMPORT_ROLLBACK_AND_FAIL(tag, msg);
        }
    }
#undef IMPORT_ROLLBACK_AND_FAIL

    // Apply debug_logging from the envelope.
    if (new_debug != g_debug_logging_enabled) {
        g_debug_logging_enabled = new_debug;
        if (g_axparam) {
            GError* err = NULL;
            ax_parameter_set(g_axparam, "DebugLogging",
                             new_debug ? "yes" : "no", TRUE, &err);
            if (err) {
                LOG_WARN("import: ax_parameter_set DebugLogging failed: %s",
                         err->message);
                g_error_free(err);
            }
        }
    }

    cJSON_Delete(in);

    // Step 6: trigger an explicit recompute with import trigger so the
    // ring reflects the cause. anchors_replace_all already triggered a
    // CONFIG_CHANGE recompute; we do one more for the IMPORT label.
    timers_recompute_now(RECOMPUTE_TRIGGER_IMPORT);

    // Build success response.
    cJSON* resp = cJSON_CreateObject();
    cJSON* imp = cJSON_AddObjectToObject(resp, "imported");
    cJSON_AddNumberToObject(imp, "anchors",                n_anchors);
    cJSON_AddNumberToObject(imp, "calendar",               n_cal);
    cJSON_AddNumberToObject(imp, "schedule_enabled_keys",  n_en);
    const recompute_summary_t* last = status_last();
    if (last) {
        cJSON_AddItemToObject(resp, "recompute", summary_to_json(last));
    } else {
        cJSON_AddNullToObject(resp, "recompute");
    }
    ACAP_HTTP_Respond_JSON(response, resp);
    cJSON_Delete(resp);
}

// Helper: persist a debug_logging change through AXParameter and the
// in-memory flag. Returns 0 on success.
static int set_debug_logging_persistent(int enabled) {
    g_debug_logging_enabled = enabled ? 1 : 0;
    if (!g_axparam) {
        LOG_WARN("set_debug_logging: AXParameter not initialized");
        return -1;
    }
    GError* err = NULL;
    if (!ax_parameter_set(g_axparam, "DebugLogging",
                          enabled ? "yes" : "no", TRUE, &err)) {
        LOG_WARN("ax_parameter_set DebugLogging failed: %s",
                 err ? err->message : "(no message)");
        if (err) g_error_free(err);
        return -1;
    }
    return 0;
}

static void HTTP_Endpoint_Debug(const ACAP_HTTP_Response response,
                                const ACAP_HTTP_Request  request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method) {
        respond_json_error(response, 400, "bad_request", "missing method");
        return;
    }

    if (strcmp(method, "GET") == 0) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "debug_logging", g_debug_logging_enabled);
        cJSON_AddItemToObject(o, "axparameters", build_axparams_json());
        ACAP_HTTP_Respond_JSON(response, o);
        cJSON_Delete(o);
        return;
    }

    if (strcmp(method, "POST") == 0) {
        if (!request->postData) {
            respond_json_error(response, 400, "bad_request", "missing body");
            return;
        }
        cJSON* in = cJSON_Parse(request->postData);
        if (!in) {
            respond_json_error(response, 400, "bad_request",
                               "body is not valid JSON");
            return;
        }
        cJSON* v = cJSON_GetObjectItem(in, "debug_logging");
        if (!cJSON_IsBool(v)) {
            cJSON_Delete(in);
            respond_json_error(response, 400, "bad_request",
                               "body must include boolean 'debug_logging'");
            return;
        }
        int en = cJSON_IsTrue(v) ? 1 : 0;
        cJSON_Delete(in);
        if (set_debug_logging_persistent(en) != 0) {
            respond_json_error(response, 500, "param_set_failed",
                               "could not persist DebugLogging");
            return;
        }
        cJSON* o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "debug_logging", g_debug_logging_enabled);
        cJSON_AddItemToObject(o, "axparameters", build_axparams_json());
        ACAP_HTTP_Respond_JSON(response, o);
        cJSON_Delete(o);
        return;
    }

    respond_json_error(response, 405, "method_not_allowed", "use GET or POST");
}

// ---- AXParameter setup (FR-12.2) ----------------------------------

// Read a parameter as int with bounds [min,max]; if missing or invalid,
// keep the existing default. `out` is updated only on a clean read.
static void axparam_load_int(AXParameter* p, const char* name,
                             int min_v, int max_v, int* out) {
    if (!p || !name || !out) return;
    GError* err = NULL;
    gchar* val = NULL;
    if (!ax_parameter_get(p, name, &val, &err)) {
        if (err) g_error_free(err);
        return;
    }
    if (val) {
        char* endptr = NULL;
        long parsed = strtol(val, &endptr, 10);
        if (endptr && *endptr == '\0' && parsed >= min_v && parsed <= max_v) {
            *out = (int)parsed;
        }
        g_free(val);
    }
}

// Load a string AXParameter into a fixed-size buffer. snprintf
// truncates to (out_len - 1) silently — the AXParameter type string
// "string:maxlen=N" is enforced server-side on writes via param.cgi
// per Axis docs, so a 33-byte buffer paired with maxlen=32 has a
// guaranteed safe round-trip at runtime, but the truncation here
// catches any pre-M7 values written before the type string was added.
static void axparam_load_string(AXParameter* p, const char* name,
                                char* out, size_t out_len) {
    if (!p || !name || !out || out_len == 0) return;
    GError* err = NULL;
    gchar* val = NULL;
    if (!ax_parameter_get(p, name, &val, &err)) {
        if (err) g_error_free(err);
        return;
    }
    if (val) {
        snprintf(out, out_len, "%s", val);
        g_free(val);
    }
}

static void axparam_load_bool_yesno(AXParameter* p, const char* name, int* out) {
    if (!p || !name || !out) return;
    GError* err = NULL;
    gchar* val = NULL;
    if (!ax_parameter_get(p, name, &val, &err)) {
        if (err) g_error_free(err);
        return;
    }
    if (val) {
        // Accept yes/no plus true/false for forward-compat.
        if (g_ascii_strcasecmp(val, "yes")  == 0 ||
            g_ascii_strcasecmp(val, "true") == 0) {
            *out = 1;
        } else if (g_ascii_strcasecmp(val, "no")    == 0 ||
                   g_ascii_strcasecmp(val, "false") == 0) {
            *out = 0;
        }
        g_free(val);
    }
}

// AXParameter callback: fires whenever any parameter under our package
// namespace is changed via param.cgi or the Axis Web UI's parameter
// editor. We can't filter by name without inspecting the `name`
// argument; the callback signature delivers the full path.
static void on_axparam_changed(const gchar* name, const gchar* value,
                               gpointer user_data) {
    (void)user_data;
    if (!name) return;

    // The framework delivers fully-qualified names like
    // "root.camera_schedule.DebugLogging". Strip the prefix.
    const char* leaf = strrchr(name, '.');
    leaf = leaf ? leaf + 1 : name;

    LOG("AXParameter changed: %s='%s'", leaf, value ? value : "(null)");

    if (strcmp(leaf, "DebugLogging") == 0) {
        if (value && (g_ascii_strcasecmp(value, "yes")  == 0 ||
                      g_ascii_strcasecmp(value, "true") == 0))
            g_debug_logging_enabled = 1;
        else
            g_debug_logging_enabled = 0;
        return;
    }
    if (strcmp(leaf, "LookaheadDays") == 0 && value) {
        long parsed = strtol(value, NULL, 10);
        if (parsed >= 1 && parsed <= 30) g_param_lookahead_days = (int)parsed;
        return;
    }
    if (strcmp(leaf, "PollIntervalSeconds") == 0 && value) {
        long parsed = strtol(value, NULL, 10);
        if (parsed >= 30 && parsed <= 600) g_param_poll_interval_seconds = (int)parsed;
        return;
    }
    if (strcmp(leaf, "EventNamePrefix") == 0 && value) {
        snprintf(g_param_event_name_prefix,
                 sizeof g_param_event_name_prefix, "%s", value);
        return;
    }
}

// Idempotent "add or get" for one parameter. ax_parameter_add fails
// when the parameter already exists; we tolerate that and proceed to
// read the current value via axparam_load_*.
static void axparam_ensure(AXParameter* p, const char* name,
                           const char* initial_value, const char* type) {
    if (!p || !name) return;
    GError* err = NULL;
    if (!ax_parameter_add(p, name, initial_value, type, &err)) {
        // Common case after first boot: parameter already exists. The
        // Axis SDK sets err->code == AX_PARAMETER_PARAM_ADDED_ERROR
        // for that, but the symbol name varies across SDK versions —
        // err->message reliably contains "already" on those builds.
        if (err && err->message && strstr(err->message, "already") == NULL) {
            LOG_WARN("ax_parameter_add(%s) failed: %s", name, err->message);
        }
        if (err) g_error_free(err);
    }
    GError* cb_err = NULL;
    if (!ax_parameter_register_callback(p, name, on_axparam_changed,
                                        NULL, &cb_err)) {
        if (cb_err) {
            LOG_WARN("ax_parameter_register_callback(%s) failed: %s",
                     name, cb_err->message ? cb_err->message : "(no msg)");
            g_error_free(cb_err);
        }
    }
}

static void axparams_init(void) {
    GError* err = NULL;
    g_axparam = ax_parameter_new(APP_PACKAGE, &err);
    if (!g_axparam) {
        LOG_WARN("ax_parameter_new failed: %s",
                 err ? err->message : "(no message)");
        if (err) g_error_free(err);
        return;
    }

    // Declare the four scalars (idempotent across boots). Defaults must
    // match contract §2.1.
    axparam_ensure(g_axparam, "LookaheadDays",        "7",  "int:min=1,max=30");
    axparam_ensure(g_axparam, "EventNamePrefix",      "",   "string:maxlen=32");
    axparam_ensure(g_axparam, "PollIntervalSeconds",  "60", "int:min=30,max=600");
    axparam_ensure(g_axparam, "DebugLogging",         "no", "bool:no,yes");

    // Seed in-memory mirrors from current values (operator may have
    // changed any of them via param.cgi between runs).
    axparam_load_int(g_axparam,    "LookaheadDays",
                     1, 30, &g_param_lookahead_days);
    axparam_load_string(g_axparam, "EventNamePrefix",
                        g_param_event_name_prefix,
                        sizeof g_param_event_name_prefix);
    axparam_load_int(g_axparam,    "PollIntervalSeconds",
                     30, 600, &g_param_poll_interval_seconds);
    axparam_load_bool_yesno(g_axparam, "DebugLogging", &g_debug_logging_enabled);

    LOG("AXParameter init: LookaheadDays=%d EventNamePrefix='%s' "
        "PollIntervalSeconds=%d DebugLogging=%s",
        g_param_lookahead_days, g_param_event_name_prefix,
        g_param_poll_interval_seconds,
        g_debug_logging_enabled ? "yes" : "no");

    // EventNamePrefix is declared and persisted but NOT YET wired into
    // event registration. Per contract §8 the prefix should apply at
    // boot to the AXEvent NiceName for every declared topic
    // (ACAP_EVENTS_Add_Event in the seed loop in main()) and in
    // apply_seasonal_labels(). Wiring it requires touching code paths
    // shared with the UI agent's branch and was deferred to keep M7's
    // SSE diff minimal. Operator changes to this parameter will simply
    // be ignored at boot until that wiring lands.
    if (g_param_event_name_prefix[0] != '\0')
        LOG("Note: EventNamePrefix='%s' set but not yet applied to topic NiceNames",
            g_param_event_name_prefix);
}

static void axparams_cleanup(void) {
    if (g_axparam) {
        ax_parameter_free(g_axparam);
        g_axparam = NULL;
    }
}

// ---- Hemisphere-aware solstice labels (FR-5.2) --------------------

// FR-5.2 says solstice labels SHALL be derived from the sign of the
// camera's latitude:
//   * Northern (lat >= +0.5°): June → "Longest Day", Dec → "Shortest Day"
//   * Southern (lat <= -0.5°): inverse
//   * Equatorial (|lat| < 0.5°): neutral "June solstice" / "December solstice"
//
// settings/events.json is loaded by ACAP() at boot with the neutral
// labels. After ACAP() returns we know lat, so we Remove + Add the
// two solstice topics with the hemisphere-appropriate nice names.
// Same helper is called from the location POST handler so an operator
// who moves the camera across hemispheres gets the correct labels
// without rebooting. Action rules in Axis bind by topic ID, not nice
// name, so Remove + Add is safe for any pre-bound rules.
static void apply_seasonal_labels(double lat) {
    const char* june_name;
    const char* dec_name;

    if (lat >= 0.5) {
        june_name = "Longest Day";
        dec_name  = "Shortest Day";
    } else if (lat <= -0.5) {
        june_name = "Shortest Day";
        dec_name  = "Longest Day";
    } else {
        june_name = "June solstice";
        dec_name  = "December solstice";
    }

    // FR-5.3: equinoxes always render as the neutral "March equinox" /
    // "September equinox" — no rebinding needed for those two.
    ACAP_EVENTS_Remove_Event("junesolstice");
    ACAP_EVENTS_Add_Event("junesolstice", june_name, 0);
    ACAP_EVENTS_Remove_Event("decembersolstice");
    ACAP_EVENTS_Add_Event("decembersolstice", dec_name, 0);

    LOG("Seasonal labels applied for lat=%f: junesolstice='%s', decembersolstice='%s'",
        lat, june_name, dec_name);
}

// ---- Settings callback (config persistence; unused at M2) ---------

static void Settings_Updated_Callback(const char* service, cJSON* data) {
    (void)service;
    (void)data;
    // No persisted settings yet. Hook is in place so the framework
    // can call it without crashing.
}

// ---- Boot ---------------------------------------------------------

int main(void) {
    openlog(APP_PACKAGE, LOG_PID | LOG_CONS, LOG_USER);
    LOG("------ Starting %s v%s (%s) ------", APP_PACKAGE, APP_VERSION, APP_ARCH);

    // ACAP() initializes HTTP/FastCGI, AXParameter, status, VAPIX,
    // AXEvent, ACAP_DEVICE_*, and reads settings/events.json to declare
    // the event topics with the AXEvent handler.
    ACAP(APP_PACKAGE, Settings_Updated_Callback);

    // M7: status ring buffer (FR-13.3) and AXParameter scalars
    // (FR-12.2). Both come up before timers_init() so the boot-time
    // recompute records correctly and reads any operator-set lookahead.
    status_init();
    axparams_init();

    ACAP_HTTP_Node("about",         HTTP_Endpoint_About);
    ACAP_HTTP_Node("location",      HTTP_Endpoint_Location);
    ACAP_HTTP_Node("anchors",       HTTP_Endpoint_Anchors);
    ACAP_HTTP_Node("calendar",      HTTP_Endpoint_Calendar);
    ACAP_HTTP_Node("events",        HTTP_Endpoint_Events);
    ACAP_HTTP_Node("events_today",  HTTP_Endpoint_Events_Today);
    ACAP_HTTP_Node("status",        HTTP_Endpoint_Status);
    ACAP_HTTP_Node("recompute",     HTTP_Endpoint_Recompute);
    ACAP_HTTP_Node("export",        HTTP_Endpoint_Export);
    ACAP_HTTP_Node("import",        HTTP_Endpoint_Import);
    ACAP_HTTP_Node("debug",         HTTP_Endpoint_Debug);

    // Re-label the two solstice topics now that lat is known. Must
    // happen after ACAP() (which declares the topics with their
    // neutral defaults from events.json) but before timers_init() so
    // the AXEvent declarations are settled before any timer can fire.
    apply_seasonal_labels(ACAP_DEVICE_Latitude());

    // M6: load anchor + calendar config and declare their topics
    // BEFORE timers_init so the recompute can arm them on the first
    // pass. Both modules tolerate missing files (clean install) and
    // quarantine malformed ones per FR-12.4.
    if (anchors_init() != 0)
        LOG_WARN("anchors_init failed; built-in topics still declared by ACAP()");
    if (calendar_init() != 0)
        LOG_WARN("calendar_init failed; calendar entries unavailable");

    // Declare AXEvent topics for the operator anchors and calendar
    // entries loaded above. Built-in topics were declared by ACAP() at
    // boot via settings/events.json.
    {
        size_t na = anchors_count();
        for (size_t i = 0; i < na; i++) {
            anchor_t a;
            if (anchors_get_by_index(i, &a) != 0) continue;
            if (a.built_in) continue;
            int stateful = (a.kind == ANCHOR_KIND_PAIRED) ||
                           (a.kind == ANCHOR_KIND_OFFSET && a.duration_minutes > 0);
            ACAP_EVENTS_Add_Event(a.id, a.name, stateful);
        }
        size_t nc = calendar_count();
        for (size_t i = 0; i < nc; i++) {
            calendar_entry_t e;
            if (calendar_get_by_index(i, &e) != 0) continue;
            int stateful = (e.time_mode == CALENDAR_TIME_ALL_DAY);
            ACAP_EVENTS_Add_Event(e.id, e.name, stateful);
        }
    }

    // Arm the daily-recompute machinery and today's per-event timers.
    if (timers_init() != 0)
        LOG_WARN("timers_init failed; events will not fire until "
                 "geolocation is configured and a recompute is triggered");

    main_loop = g_main_loop_new(NULL, FALSE);

    GSource* sigterm_src = g_unix_signal_source_new(SIGTERM);
    if (sigterm_src) {
        g_source_set_callback(sigterm_src, signal_handler, NULL, NULL);
        g_source_attach(sigterm_src, NULL);
    } else {
        LOG_WARN("Failed to install SIGTERM handler");
    }
    GSource* sigint_src = g_unix_signal_source_new(SIGINT);
    if (sigint_src) {
        g_source_set_callback(sigint_src, signal_handler, NULL, NULL);
        g_source_attach(sigint_src, NULL);
    }

    LOG("Entering main loop");
    g_main_loop_run(main_loop);

    LOG("------ Exit %s ------", APP_PACKAGE);
    timers_cleanup();
    calendar_cleanup();
    anchors_cleanup();
    axparams_cleanup();
    ACAP_Cleanup();
    g_main_loop_unref(main_loop);
    closelog();
    return 0;
}
