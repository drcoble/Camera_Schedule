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
#include <glib.h>
#include <glib-unix.h>

#include "acap/ACAP.h"
#include "acap/cJSON.h"
#include "anchors.h"
#include "calendar.h"
#include "timers.h"
#include "astro/solar.h"
#include "astro/lunar.h"
#include "astro/seasonal.h"

#define APP_PACKAGE "camera_schedule"
#define APP_VERSION "0.6.0"

#if defined(__aarch64__)
#define APP_ARCH "aarch64"
#elif defined(__arm__)
#define APP_ARCH "armv7hf"
#else
#define APP_ARCH "unknown"
#endif

#define LOG(fmt, args...)      do { syslog(LOG_INFO,    fmt, ## args); } while (0)
#define LOG_WARN(fmt, args...) do { syslog(LOG_WARNING, fmt, ## args); } while (0)

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
        timers_recompute_now();

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
    timers_recompute_now();

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
    int lookahead_days = 1;
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

    ACAP_HTTP_Node("about",         HTTP_Endpoint_About);
    ACAP_HTTP_Node("location",      HTTP_Endpoint_Location);
    ACAP_HTTP_Node("anchors",       HTTP_Endpoint_Anchors);
    ACAP_HTTP_Node("calendar",      HTTP_Endpoint_Calendar);
    ACAP_HTTP_Node("events",        HTTP_Endpoint_Events);
    ACAP_HTTP_Node("events_today",  HTTP_Endpoint_Events_Today);

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
    ACAP_Cleanup();
    g_main_loop_unref(main_loop);
    closelog();
    return 0;
}
