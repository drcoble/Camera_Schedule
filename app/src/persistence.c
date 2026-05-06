// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Camera_Schedule contributors
//
// See persistence.h for the contract.

#define _GNU_SOURCE
#include "persistence.h"
#include "acap/ACAP.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG(fmt, args...)      do { syslog(LOG_INFO,    fmt, ## args); } while (0)
#define LOG_WARN(fmt, args...) do { syslog(LOG_WARNING, fmt, ## args); } while (0)
#define LOG_ERR(fmt, args...)  do { syslog(LOG_ERR,     fmt, ## args); } while (0)

// Build "<sandbox><relative>" into `out`. Returns 0 on success, -1 on
// truncation. The sandbox prefix already ends in '/'.
static int build_full_path(char* out, size_t out_len, const char* relative) {
    const char* sandbox = ACAP_FILE_AppPath();
    if (!sandbox || !relative) return -1;
    int n = snprintf(out, out_len, "%s%s", sandbox, relative);
    if (n < 0 || (size_t)n >= out_len) return -1;
    return 0;
}

// Re-read the just-written temp file into a fresh cJSON tree. Caller
// frees with cJSON_Delete. Returns NULL on any failure.
static cJSON* parse_back(const char* full_temp_path) {
    FILE* f = fopen(full_temp_path, "r");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    char* buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(buf); return NULL; }
    buf[size] = '\0';

    cJSON* parsed = cJSON_Parse(buf);
    free(buf);
    return parsed;
}

int persistence_write_atomic(const char*             relative_path,
                             const cJSON*            payload,
                             persistence_validator_t validator,
                             void*                   user_data) {
    if (!relative_path || !payload) return -1;

    char final_path[256];
    char temp_path[256];
    if (build_full_path(final_path, sizeof final_path, relative_path) != 0) {
        LOG_ERR("persistence: relative path '%s' too long", relative_path);
        return -1;
    }
    int n = snprintf(temp_path, sizeof temp_path, "%s.tmp", final_path);
    if (n < 0 || (size_t)n >= sizeof temp_path) {
        LOG_ERR("persistence: temp path for '%s' too long", relative_path);
        return -1;
    }

    // Serialize. cJSON_Print returns a malloc'd string; caller frees.
    char* serialized = cJSON_Print((cJSON*)payload);
    if (!serialized) {
        LOG_ERR("persistence: cJSON_Print failed for '%s'", relative_path);
        return -1;
    }

    // Open the temp file with O_CREAT|O_TRUNC. Use fdopen for fsync.
    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        LOG_ERR("persistence: open(%s) failed: %s", temp_path, strerror(errno));
        free(serialized);
        return -1;
    }

    size_t to_write = strlen(serialized);
    size_t written = 0;
    while (written < to_write) {
        ssize_t w = write(fd, serialized + written, to_write - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            LOG_ERR("persistence: write(%s) failed: %s", temp_path, strerror(errno));
            close(fd);
            unlink(temp_path);
            free(serialized);
            return -1;
        }
        written += (size_t)w;
    }

    if (fsync(fd) != 0) {
        LOG_ERR("persistence: fsync(%s) failed: %s", temp_path, strerror(errno));
        close(fd);
        unlink(temp_path);
        free(serialized);
        return -1;
    }
    close(fd);
    free(serialized);

    // Parse-back validation. Skipped if no validator supplied.
    if (validator) {
        cJSON* parsed = parse_back(temp_path);
        if (!parsed) {
            LOG_ERR("persistence: parse-back of '%s' failed", temp_path);
            unlink(temp_path);
            return -1;
        }
        int ok = validator(parsed, user_data);
        cJSON_Delete(parsed);
        if (!ok) {
            LOG_ERR("persistence: schema validation rejected '%s'", relative_path);
            unlink(temp_path);
            return -1;
        }
    }

    // Atomic rename — POSIX guarantees this is atomic on the same
    // filesystem (which is the case inside the ACAP sandbox).
    if (rename(temp_path, final_path) != 0) {
        LOG_ERR("persistence: rename(%s -> %s) failed: %s",
                temp_path, final_path, strerror(errno));
        unlink(temp_path);
        return -1;
    }

    LOG("persistence: wrote %s atomically", relative_path);
    return 0;
}

int persistence_quarantine(const char* relative_path) {
    if (!relative_path) return -1;
    char full_path[256];
    char broken_path[300];
    if (build_full_path(full_path, sizeof full_path, relative_path) != 0) return -1;

    long long ts = (long long)time(NULL);
    int n = snprintf(broken_path, sizeof broken_path,
                     "%s.broken-%lld", full_path, ts);
    if (n < 0 || (size_t)n >= sizeof broken_path) return -1;

    if (rename(full_path, broken_path) != 0) {
        LOG_ERR("persistence: quarantine rename(%s -> %s) failed: %s",
                full_path, broken_path, strerror(errno));
        return -1;
    }
    LOG_ERR("persistence: quarantined malformed file '%s' as '%s'",
            relative_path, broken_path);
    return 0;
}
