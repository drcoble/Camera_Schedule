#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Camera_Schedule contributors
#
# soak_common.sh — shared harness sourced by soak_1h.sh and soak_24h.sh.
#
# Required environment variables (set by caller or inherited):
#   AXIS_HOST_OS12   — IP or hostname of the OS 12 lab camera
#   AXIS_PASS        — root password
#   WINDOW           — soak duration in seconds (3600 = 1 h, 86400 = 24 h)
#
# The harness produces two artifacts in ./soak/ (relative to $PWD):
#   soak/<startdate>.ndjson          — one JSON object per sample line
#   soak/<startdate>-report.md       — summary Markdown report
#
# RSS sampling mechanism (see docs/soak.md for rationale):
#   Priority 1: In-app /status endpoint rss_kb field (OQ-15 proposal).
#   If neither is available, RSS tracking is skipped and noted in report.
#
# SSH is NOT used: Axis cameras ship with SSH disabled by default, and
# BatchMode SSH (no password prompt) cannot authenticate with a password.
# See DL-21 for why rss_kb in /status is the chosen mechanism.
#
# Failure thresholds (§6.2 of M7_API_CONTRACT.md):
#   - RSS grows > 20% above minute-5 baseline
#   - Any AppArmor DENIED entry referencing camera_schedule
#   - GET /status returns non-200 for > 3 consecutive samples
#   - last_recompute.errors ever > 0
#
# Usage (don't call directly — use soak_1h.sh or soak_24h.sh):
#   WINDOW=3600 AXIS_HOST_OS12=... AXIS_PASS=... . ./soak_common.sh
#   soak_run

set -e

# ---------------------------------------------------------------------------
# Validate required variables
# ---------------------------------------------------------------------------
soak_check_env() {
    local missing=0
    if [ -z "${AXIS_HOST_OS12:-}" ]; then
        echo "ERROR: AXIS_HOST_OS12 is not set" >&2
        missing=1
    fi
    if [ -z "${AXIS_PASS:-}" ]; then
        echo "ERROR: AXIS_PASS is not set" >&2
        missing=1
    fi
    if [ -z "${WINDOW:-}" ]; then
        echo "ERROR: WINDOW (seconds) is not set" >&2
        missing=1
    fi
    if [ "$missing" -ne 0 ]; then
        echo "Set these from the project memory file (not in the repo)." >&2
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# Detect RSS sampling mechanism (read-only camera probes)
# ---------------------------------------------------------------------------
soak_detect_rss_method() {
    # Try the /status endpoint's rss_kb field (OQ-15 proposal).
    # SSH is not used: Axis cameras ship with SSH disabled by default, and
    # BatchMode SSH cannot authenticate with a password credential.
    # See DL-21 and docs/soak.md for rationale.
    local status_body
    status_body=$(curl -sk --anyauth \
        -u "root:${AXIS_PASS}" \
        "https://${AXIS_HOST_OS12}/local/camera_schedule/status" 2>/dev/null || true)
    if echo "${status_body}" | grep -q '"rss_kb"'; then
        RSS_METHOD="status_rss_kb"
        echo "[soak] RSS method: /status.rss_kb field"
        return 0
    fi

    RSS_METHOD="none"
    echo "[soak] WARNING: /status lacks rss_kb field (OQ-15 not yet landed)." \
         "RSS leak detection is disabled for this run."
}

# ---------------------------------------------------------------------------
# Sample RSS using the detected method
# ---------------------------------------------------------------------------
soak_sample_rss() {
    case "${RSS_METHOD}" in
        status_rss_kb)
            # Extract rss_kb from /status JSON (OQ-15 field, see DL-21).
            local body
            body=$(curl -sk --anyauth \
                -u "root:${AXIS_PASS}" \
                "https://${AXIS_HOST_OS12}/local/camera_schedule/status" 2>/dev/null || echo "{}")
            echo "$body" | grep -o '"rss_kb":[0-9]*' | grep -o '[0-9]*$' || echo 0
            ;;
        *)
            echo 0
            ;;
    esac
}

# ---------------------------------------------------------------------------
# Sample AppArmor DENIED entries (read-only, VAPIX syslog)
# ---------------------------------------------------------------------------
soak_sample_apparmor() {
    # VAPIX system_log.cgi returns the last N syslog lines.
    # Parse for DENIED entries referencing camera_schedule.
    curl -sk --anyauth \
        -u "root:${AXIS_PASS}" \
        "https://${AXIS_HOST_OS12}/axis-cgi/admin/system_log.cgi" 2>/dev/null \
        | grep -c "DENIED.*camera_schedule\|camera_schedule.*DENIED" 2>/dev/null \
        || echo 0
}

# ---------------------------------------------------------------------------
# GET /status (returns full body or empty string on error)
# ---------------------------------------------------------------------------
soak_get_status() {
    local http_code body
    # Write HTTP status code and body to separate variables.
    body=$(curl -sk --anyauth \
        -u "root:${AXIS_PASS}" \
        -o /dev/null \
        -w "%{http_code}" \
        "https://${AXIS_HOST_OS12}/local/camera_schedule/status" 2>/dev/null \
        || echo "000")
    echo "$body"
}

soak_get_status_body() {
    curl -sk --anyauth \
        -u "root:${AXIS_PASS}" \
        "https://${AXIS_HOST_OS12}/local/camera_schedule/status" 2>/dev/null \
        || echo "{}"
}

# ---------------------------------------------------------------------------
# Main soak loop
# ---------------------------------------------------------------------------
soak_run() {
    soak_check_env
    soak_detect_rss_method

    local start_epoch
    start_epoch=$(date -u +%s)
    local start_date
    start_date=$(date -u +%Y%m%dT%H%M%SZ)

    mkdir -p "./soak"
    local ndjson_file="./soak/${start_date}.ndjson"
    local report_file="./soak/${start_date}-report.md"

    echo "[soak] Start: ${start_date}  Window: ${WINDOW}s  Host: ${AXIS_HOST_OS12}"
    echo "[soak] NDJSON: ${ndjson_file}"
    echo "[soak] Report: ${report_file}"

    local sample=0
    local rss_baseline=0
    local consecutive_non200=0
    local apparmor_total=0
    local max_rss=0
    local rss_failures=0
    local apparmor_failures=0
    local status_failures=0
    local recompute_error_failures=0
    local total_samples=0
    local run_failed=0
    local failure_reasons=""

    # Write report header
    cat > "${report_file}" <<REPHEAD
# Camera_Schedule Soak Run Report

- **Host**: ${AXIS_HOST_OS12}
- **Started**: ${start_date}
- **Window**: ${WINDOW} s
- **RSS method**: ${RSS_METHOD}

## Failure thresholds
- RSS growth > 20% above minute-5 baseline
- AppArmor DENIED entries referencing camera_schedule: any → fail
- GET /status non-200 for > 3 consecutive samples
- last_recompute.errors > 0

## Per-sample log

| Sample | UTC | RSS (kB) | AA denied | /status | recompute.errors |
|--------|-----|----------|-----------|---------|-----------------|
REPHEAD

    while true; do
        local now_epoch
        now_epoch=$(date -u +%s)
        local elapsed=$(( now_epoch - start_epoch ))

        if [ "$elapsed" -ge "$WINDOW" ]; then
            break
        fi

        local now_ts
        now_ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
        sample=$(( sample + 1 ))
        total_samples=$sample

        # --- RSS ---
        local rss_kb=0
        rss_kb=$(soak_sample_rss)
        rss_kb=$(echo "$rss_kb" | tr -d '[:space:]' | grep -o '^[0-9]*' || echo 0)
        [ -z "$rss_kb" ] && rss_kb=0

        # Set baseline at ~5 minute mark (sample 5 at 60s interval)
        if [ "$sample" -eq 5 ] && [ "$rss_baseline" -eq 0 ] && [ "$rss_kb" -gt 0 ]; then
            rss_baseline=$rss_kb
            echo "[soak] RSS baseline set at sample 5: ${rss_baseline} kB"
        fi
        if [ "$rss_kb" -gt "$max_rss" ]; then
            max_rss=$rss_kb
        fi

        # RSS leak check: only after baseline is set
        local rss_flag=""
        if [ "$rss_baseline" -gt 0 ] && [ "$rss_kb" -gt 0 ]; then
            # check if rss_kb > rss_baseline * 1.20 (20% growth)
            local threshold=$(( rss_baseline + rss_baseline / 5 ))
            if [ "$rss_kb" -gt "$threshold" ]; then
                rss_failures=$(( rss_failures + 1 ))
                run_failed=1
                rss_flag="FAIL"
                failure_reasons="${failure_reasons}\n- Sample ${sample}: RSS ${rss_kb} kB > 120% of baseline ${rss_baseline} kB"
            fi
        fi

        # --- AppArmor ---
        local aa_count=0
        aa_count=$(soak_sample_apparmor)
        aa_count=$(echo "$aa_count" | tr -d '[:space:]' | grep -o '^[0-9]*' || echo 0)
        [ -z "$aa_count" ] && aa_count=0
        if [ "$aa_count" -gt 0 ]; then
            apparmor_total=$(( apparmor_total + aa_count ))
            apparmor_failures=$(( apparmor_failures + 1 ))
            run_failed=1
            failure_reasons="${failure_reasons}\n- Sample ${sample}: AppArmor DENIED count ${aa_count}"
        fi

        # --- GET /status ---
        local http_code
        http_code=$(soak_get_status)
        local status_body="{}"
        local recompute_errors=0
        local status_flag=""

        if [ "$http_code" = "200" ]; then
            consecutive_non200=0
            status_body=$(soak_get_status_body)
            # Extract last_recompute.errors
            recompute_errors=$(echo "$status_body" \
                | grep -o '"errors":[0-9]*' | head -1 | grep -o '[0-9]*$' || echo 0)
            [ -z "$recompute_errors" ] && recompute_errors=0
            if [ "$recompute_errors" -gt 0 ]; then
                recompute_error_failures=$(( recompute_error_failures + 1 ))
                run_failed=1
                failure_reasons="${failure_reasons}\n- Sample ${sample}: last_recompute.errors=${recompute_errors}"
            fi
        else
            consecutive_non200=$(( consecutive_non200 + 1 ))
            if [ "$consecutive_non200" -gt 3 ]; then
                status_failures=$(( status_failures + 1 ))
                run_failed=1
                status_flag="FAIL"
                failure_reasons="${failure_reasons}\n- Sample ${sample}: GET /status returned ${http_code} (${consecutive_non200} consecutive non-200)"
            fi
        fi

        # --- Write ND-JSON line ---
        printf '{"sample":%d,"ts":"%s","rss_kb":%d,"aa_denied":%d,"http_status":"%s","recompute_errors":%d}\n' \
            "$sample" "$now_ts" "$rss_kb" "$aa_count" "$http_code" "$recompute_errors" \
            >> "${ndjson_file}"

        # --- Write report table row ---
        printf "| %d | %s | %d%s | %d | %s | %d |\n" \
            "$sample" "$now_ts" "$rss_kb" "${rss_flag:+ ($rss_flag)}" \
            "$aa_count" "$http_code" "$recompute_errors" \
            >> "${report_file}"

        echo "[soak] Sample ${sample} ts=${now_ts} rss=${rss_kb}kB aa=${aa_count} http=${http_code} rc_err=${recompute_errors}"

        sleep 60
    done

    # Finalize report
    local end_ts
    end_ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    local status_word="PASS"
    [ "$run_failed" -ne 0 ] && status_word="FAIL"

    cat >> "${report_file}" <<REPFOOTER

## Summary

- **Ended**: ${end_ts}
- **Total samples**: ${total_samples}
- **RSS baseline**: ${rss_baseline} kB
- **RSS max observed**: ${max_rss} kB
- **RSS failure samples**: ${rss_failures}
- **AppArmor failure samples**: ${apparmor_failures}
- **GET /status consecutive-non-200 events**: ${status_failures}
- **Recompute error events**: ${recompute_error_failures}

## Result: **${status_word}**
REPFOOTER

    if [ "$run_failed" -ne 0 ]; then
        printf "\n## Failure details\n" >> "${report_file}"
        printf "%b\n" "$failure_reasons" >> "${report_file}"
    fi

    echo ""
    echo "[soak] ============================================"
    echo "[soak] Run complete. Result: ${status_word}"
    echo "[soak] Report: ${report_file}"
    echo "[soak] NDJSON: ${ndjson_file}"
    echo "[soak] ============================================"

    if [ "$run_failed" -ne 0 ]; then
        return 1
    fi
    return 0
}
