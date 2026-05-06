// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Single source of the LOG_* family. Lifted in M7 from the previously
// scattered `#define LOG ...` blocks in main.c / timers.c / persistence.c
// so the LOG_DBG runtime gate (FR-13.4) has exactly one place to read
// `g_debug_logging_enabled` from.
//
// All four levels go through `syslog(3)` per FR-13.1; the program
// identifier is established via `openlog()` in main.c.
//
// LOG_DBG is gated by `g_debug_logging_enabled`, which is seeded at boot
// from the `DebugLogging` AXParameter and flipped by the POST /debug
// endpoint and the AXParameter callback (FR-13.4 / contract §2.2).

#ifndef CAMERA_SCHEDULE_LOG_H
#define CAMERA_SCHEDULE_LOG_H

#include <syslog.h>

#ifdef __cplusplus
extern "C" {
#endif

// Defined in main.c. Touched only on the GLib main loop or behind the
// param.cgi callback (which marshals through ax_parameter_register_callback
// onto its own thread); the variable is `int` (single-word read) so a
// data race here at most flips a debug toggle a few microseconds late.
extern int g_debug_logging_enabled;

#define LOG(fmt, args...)       do { syslog(LOG_INFO,    fmt, ## args); } while (0)
#define LOG_WARN(fmt, args...)  do { syslog(LOG_WARNING, fmt, ## args); } while (0)
#define LOG_ERROR(fmt, args...) do { syslog(LOG_ERR,     fmt, ## args); } while (0)
#define LOG_DBG(fmt, args...) do { \
    if (g_debug_logging_enabled) syslog(LOG_DEBUG, fmt, ## args); \
} while (0)

#ifdef __cplusplus
}
#endif

#endif  // CAMERA_SCHEDULE_LOG_H
