// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// M1 application entry point. Boots the GLib main loop, initializes the
// vendored ACAP framework (HTTP/FastCGI registration, AXParameter,
// AXEvent, status JSON), and registers a single read-only `about`
// endpoint. No event topics are declared at this milestone — that
// arrives in M2 with the sunrise/sunset MVP.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <glib.h>
#include <glib-unix.h>

#include "acap/ACAP.h"
#include "acap/cJSON.h"

#define APP_PACKAGE "camera_schedule"
#define APP_VERSION "0.1.0"

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
    if (main_loop)
        g_main_loop_quit(main_loop);
    return G_SOURCE_REMOVE;
}

// GET /local/camera_schedule/about → {"name", "version", "arch"}
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

static void Settings_Updated_Callback(const char* service, cJSON* data) {
    (void)service;
    (void)data;
    // No persisted settings yet (M2 lands location config). Hook is in
    // place so the framework can call it without crashing.
}

int main(void) {
    openlog(APP_PACKAGE, LOG_PID | LOG_CONS, LOG_USER);
    LOG("------ Starting %s v%s (%s) ------", APP_PACKAGE, APP_VERSION, APP_ARCH);

    // Initializes HTTP/FastCGI dispatch, AXParameter, status store,
    // VAPIX helpers, AXEvent handler, and ACAP_DEVICE_*.
    ACAP(APP_PACKAGE, Settings_Updated_Callback);

    ACAP_HTTP_Node("about", HTTP_Endpoint_About);

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
    ACAP_Cleanup();
    g_main_loop_unref(main_loop);
    closelog();
    return 0;
}
