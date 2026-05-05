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
#include "timers.h"

#define APP_PACKAGE "camera_schedule"
#define APP_VERSION "0.2.0"

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
        timers_recompute_now();

        cJSON* body = current_location_json();
        ACAP_HTTP_Respond_JSON(response, body);
        cJSON_Delete(body);
        return;
    }

    ACAP_HTTP_Respond_Error(response, 405, "Method Not Allowed");
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

    ACAP_HTTP_Node("about",    HTTP_Endpoint_About);
    ACAP_HTTP_Node("location", HTTP_Endpoint_Location);

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
    ACAP_Cleanup();
    g_main_loop_unref(main_loop);
    closelog();
    return 0;
}
