# Camera_Schedule soak harness

This document describes how the soak harness (`app/test/lab/soak_1h.sh`
and `app/test/lab/soak_24h.sh`) samples the `camera_schedule` process
on the lab camera, the failure thresholds applied, and the manual
verification step for atomicity.

---

## 1. Harness overview

Both scripts source `app/test/lab/soak_common.sh` and differ only in
their `WINDOW` variable (1 h = 3600 s vs 24 h = 86400 s). Every 60 s the
harness:

1. Samples RSS of the `camera_schedule` process.
2. Checks syslog for AppArmor DENIED entries via `system_log.cgi`.
3. GETs `/local/camera_schedule/status` and records the full JSON body
   as a newline-delimited JSON (ND-JSON) line in `./soak/<startdate>.ndjson`.

At the end of the run it writes a Markdown summary to
`./soak/<startdate>-report.md`.

---

## 2. RSS sampling mechanism

### Why RSS matters

A steady RSS increase over 24 hours indicates a memory leak in the
recompute loop or the ring buffer.  A 20% growth threshold above a
stable minute-5 baseline is the accepted signal for an actionable leak.

### Mechanism selection

The harness probes at startup for the available method and commits to
it for the entire run. SSH is not used: Axis cameras ship with SSH
disabled by default, and password-authenticated SSH requires an
interactive prompt or `sshpass` — neither is available in a CI
environment. The only read-only remote path that works without SSH is
a field in the app's own `/status` response.

#### `/status` endpoint `rss_kb` field (OQ-15 proposal)

The SSE is asked to add an `rss_kb` field to the `GET /status` response
(see **OQ-15** and **DL-21** in the requirements). If the field is present,
the harness extracts it with:

```sh
curl -sk --anyauth -u "root:$AXIS_PASS" \
    "https://$AXIS_HOST_OS12/local/camera_schedule/status" \
    | grep -o '"rss_kb":[0-9]*' | grep -o '[0-9]*$'
```

This requires one extra `getrusage()` or `/proc/self/status` read in
the app's status handler — a trivial addition.

#### Fallback: no RSS tracking

If neither method is available, the harness still runs but skips RSS
monitoring. The run report notes this explicitly. RSS detection failures
do **not** cause the run to report FAIL — only the four failure conditions
in §3 below do.

### Recommended setup

Ensure the SSE has landed the `rss_kb` field in `/status` before
running the soak (see OQ-15 / DL-21).

The 1-hour pilot (`soak_1h.sh`) can run without RSS if needed since
the CI acceptance gate is primarily status-endpoint health and
AppArmor cleanliness.

---

## 3. Failure conditions

Any one of the following causes the run to exit with non-zero status
and the report to show **FAIL**:

| Condition | Threshold |
|-----------|-----------|
| RSS growth | > 20% above the minute-5 baseline sample |
| AppArmor DENIED | Any `DENIED` entry containing `camera_schedule` in syslog |
| GET /status non-200 | More than 3 consecutive non-200 responses |
| Recompute errors | `last_recompute.errors > 0` on any sample |

---

## 4. Atomicity lab check (manual)

The host fixture (`test_export_import.c §4`) documents why mid-write EIO
injection is not done in the automated test: it requires OS-level
`write()` interception not available in a pure-C host fixture.

Perform this check manually before tagging v0.7.0:

1. Trigger an import via the UI or:
   ```sh
   curl -sk --anyauth -u "root:$AXIS_PASS" \
       -X POST -H "Content-Type: application/json" \
       --data @/tmp/test_config.json \
       "https://$AXIS_HOST_OS12/local/camera_schedule/import"
   ```
2. While the import is in progress, simulate an I/O failure by filling
   the filesystem or by disconnecting power briefly (if the camera is on
   a switched outlet).
3. After the camera restarts, verify:
   - The previous `localdata/anchors.json`, `localdata/calendar.json`,
     and `localdata/schedule_enabled.json` are intact **or** that
     `localdata/anchors.json.before-import-<timestamp>` backups exist
     and the current files are consistent (not half-written).
   - The app starts cleanly with no `ERR` log entries about config load.

Expected result: the prior config is preserved; the import is rolled
back. This is guaranteed by the `persistence.c` atomic-write helper
(FR-12.1: write-temp + fsync + parse-back + schema-validate + rename).

---

## 5. Environment variables

The harness reads credentials from the environment only — never from
checked-in files. Set these before running:

```sh
export AXIS_HOST_OS12=10.1.40.113   # from project memory file
export AXIS_PASS='...'              # from project memory file
```

See `CLAUDE.md §Test cameras` for the location of the memory file.

---

## 6. Running the pilot

```sh
cd /path/to/Camera_Schedule   # repo root
export AXIS_HOST_OS12=10.1.40.113
export AXIS_PASS='...'

# 1-hour pilot (blocking; use screen/tmux for the 24-hour run)
sh app/test/lab/soak_1h.sh
```

Artifacts land in `./soak/` relative to where you run the script.
The `soak/` directory is gitignored — commit the report file manually
if you want to preserve the run history.

---

## 7. OQ-15 — RSS field in /status

**Status**: open at time of writing. Proposal sent to SSE.

If the SSE adds `rss_kb` to the `/status` response shape, the soak
harness automatically picks it up (Priority 2 path). No harness changes
are needed. The field should expose the process's VmRSS in kB as read
from `/proc/self/status` inside the status endpoint handler.

See `requirements/24-open-questions.md` OQ-15 and
`requirements/28-decision-log.md` DL-21.
