// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// Atomic JSON persistence helper used by anchors.c, calendar.c, and the
// FR-11.7 enable-state store. Wraps the FR-12.1 contract:
//
//   1. Serialize the cJSON object to bytes.
//   2. Write to "<path>.tmp" inside the ACAP sandbox.
//   3. fsync(2) the temp file.
//   4. Re-read the temp file and run a caller-supplied schema-validation
//      callback against it. If the callback rejects, abort the rename
//      and unlink the temp.
//   5. rename(2) "<path>.tmp" -> "<path>" atomically (POSIX guarantee).
//
// `relative_path` is interpreted under the ACAP sandbox (resolves via
// ACAP_FILE_AppPath), so callers pass e.g. "localdata/anchors.json".
//
// Why not just call ACAP_FILE_Write? The vendored helper does a plain
// fopen("w") with no temp-and-rename — a power loss between truncate and
// final-write loses the file. The atomic helper here meets FR-12.1.

#ifndef CAMERA_SCHEDULE_PERSISTENCE_H
#define CAMERA_SCHEDULE_PERSISTENCE_H

#include "acap/cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// Caller-supplied schema validator. Returns 1 (valid) or 0 (invalid).
// The cJSON parameter is the just-written file re-parsed from disk —
// validators MUST treat it as read-only.
typedef int (*persistence_validator_t)(const cJSON* parsed,
                                       void*        user_data);

// Write `payload` to <sandbox>/<relative_path> atomically.
//
// On success returns 0. On any failure (write, fsync, parse-back,
// validator rejection, rename) returns -1 and leaves the existing
// file (if any) untouched. The temp file is unlinked on the failure
// path.
//
// `validator` may be NULL to skip the parse-back validation step
// (the schema_enabled.json store is small enough that a per-field
// check at construction time is sufficient).
int persistence_write_atomic(const char*             relative_path,
                             const cJSON*            payload,
                             persistence_validator_t validator,
                             void*                   user_data);

// Rename a malformed file to "<relative_path>.broken-<unix-ts>" so the
// caller can start with an empty in-memory state per FR-12.4. Logs at
// LOG_ERR. Returns 0 on success, -1 on rename failure.
int persistence_quarantine(const char* relative_path);

#ifdef __cplusplus
}
#endif

#endif  // CAMERA_SCHEDULE_PERSISTENCE_H
