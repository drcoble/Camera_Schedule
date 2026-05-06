# M7 API contract — status, recompute, export/import, debug, AXParameter

This document is the single source of truth for the M7 milestone's
public surface. It is consumed by:

- the **SSE** — to implement the endpoints, the recompute ring
  buffer, the AXParameter integration, and the runtime LOG_DEBUG gate.
- the **UI designer** — to render the status panel, wire the
  Recompute / Export / Import / Debug-toggle controls, and run the
  accessibility audit.
- the **STE** — to drive host fixtures for export/import schema
  round-trip and rejection cases, and to design the soak harness.

The C-level data model is in **a new `app/src/status.h`** (SSE
authors it Phase 1-style alongside coding); this doc shows only the
parts the UI needs to render and the parts the test agent needs to
round-trip JSON.

The implementation wires the endpoints, writes `status.c`, hooks
`timers_recompute_now()` to push a summary into the ring, adds
AXParameter declarations, adds `acap-build -a` entries (none
expected — no new bundled config), and updates `manifest.json`.

Lessons from M6 are encoded as policies in §10 — read those before
starting. The two framework gotchas from
`memory/acap_framework_gotchas.md` (Respond_String 4096-byte cap;
Add_Event returns declarationID) still apply; M7 does not call
Add_Event but **does** return JSON payloads of unbounded size from
`/status` and `/export` — use the post-M6 `ACAP_HTTP_Respond_JSON`
(which now uses Respond_Data internally), never `Respond_String`
for these.

---

## 0. Conventions

- **Base path**: every M7 endpoint lives under
  `/local/camera_schedule/<endpoint>` — same prefix as M2/M6.
- **Auth**: enforced via the manifest `httpConfig.access` field
  (DL-13). Admin-write endpoints reject `POST` with HTTP 403 when the
  session role is `viewer` or `operator`.
- **Content-Type**: JSON request/response bodies, `application/json`.
  Non-JSON bodies on POST endpoints return HTTP 415.
- **Times**: UTC ISO-8601 with trailing `Z` (e.g.
  `"2026-05-31T22:14:07Z"`). Local-civil times use the camera's UTC
  offset (e.g. `"2026-05-31T18:14:07-04:00"`).
- **Error envelope**: `{"error":"<machine-tag>","message":"<human>"}`
  plus the appropriate HTTP status.

---

## 1. Endpoint reference

### 1.1 `GET /local/camera_schedule/status`

**Auth**: `viewer`-readable.

Returns the in-memory status snapshot: last recompute summary, next
scheduled recompute, current location/timezone, and the ring buffer
of recent recompute summaries (up to 50 newest-first).

```json
{
  "version": "0.7.0",
  "now": "2026-05-31T22:14:07Z",
  "now_local": "2026-05-31T18:14:07-04:00",
  "location": {
    "lat": 33.749,
    "lon": -84.388,
    "valid": true,
    "tz": "America/New_York",
    "tz_offset_seconds": -14400
  },
  "counts": {
    "topics_declared": 26,
    "topics_enabled": 24,
    "anchors_user": 4,
    "calendar_entries": 2
  },
  "last_recompute": {
    "started_at": "2026-05-31T04:30:00Z",
    "trigger": "midnight",
    "elapsed_ms": 47,
    "anchors_evaluated": 26,
    "events_armed": 23,
    "skipped_polar": 0,
    "skipped_disabled": 2,
    "skipped_past": 1,
    "errors": 0
  },
  "next_recompute": {
    "scheduled_at": "2026-06-01T04:30:00Z",
    "reason": "solar_midnight"
  },
  "recent": [
    {
      "started_at": "2026-05-31T04:30:00Z",
      "trigger": "midnight",
      "elapsed_ms": 47,
      "anchors_evaluated": 26,
      "events_armed": 23,
      "skipped_polar": 0,
      "skipped_disabled": 2,
      "skipped_past": 1,
      "errors": 0
    }
    // up to 50 entries, newest first
  ],
  "debug_logging": false,
  "axparameters": {
    "lookahead_days": 7,
    "event_name_prefix": "",
    "poll_interval_seconds": 60
  },
  "rss_kb": 4528
}
```

**`rss_kb` (DL-21)**: VmRSS read from `/proc/self/status`, in kB. `0`
when unreadable (sandboxed `/proc`, ENOENT). Used by the M7 soak
harness for memory-leak detection.

**Trigger reasons** (closed enum):
- `boot` — `timers_init` first call.
- `midnight` — daily recompute (FR-10.1, solar midnight or 02:30 fallback).
- `location_change` — POST /location (FR-10.1).
- `manual` — POST /recompute (FR-10.2).
- `config_change` — POST /anchors, POST /calendar, POST /events (anchors/calendar/enable-state edits).
- `import` — POST /import success.
- `tz_change` — detected timezone delta (FR-2.3).

**Polar / no-event-today**: when a daily recompute skips a slot
because `solar_compute()` returns SOLAR_NO_EVENT, increment
`skipped_polar`. Disabled-by-FR-11.7 skips → `skipped_disabled`.
Skipped-because-already-past → `skipped_past`.

**Errors counter**: increment on any per-slot arming failure. The
ring buffer entry's `elapsed_ms` is wall-clock duration of the
recompute scope (start of `timers_recompute_now()` → return).

### 1.2 `POST /local/camera_schedule/recompute`

**Auth**: `admin`-write.

Body: empty (`{}` accepted). Returns the new last_recompute summary
(same shape as `status.last_recompute`). Coalescing per FR-10.3:
if a recompute is already in flight, return HTTP 202 with
`{"queued": true}` and the request triggers exactly one follow-up
run.

```http
POST /local/camera_schedule/recompute
Content-Type: application/json

{}
```

Success response:

```json
{
  "started_at": "2026-05-31T22:15:00Z",
  "trigger": "manual",
  "elapsed_ms": 41,
  "anchors_evaluated": 26,
  "events_armed": 23,
  "skipped_polar": 0,
  "skipped_disabled": 2,
  "skipped_past": 1,
  "errors": 0
}
```

### 1.3 `GET /local/camera_schedule/export`

**Auth**: `admin`-readable. Per FR-12.3 export is a configuration
download — it includes the operator's full configuration, so it's
admin-gated.

Returns the full operator configuration as a self-describing
envelope. Suitable for `curl -O` or browser download.

Response headers:
```
Content-Type: application/json
Content-Disposition: attachment; filename="camera_schedule_<host>_<YYYYMMDD>.json"
```

(Host-name is best-effort; if `gethostname()` fails, omit that
segment of the filename.)

Body (envelope shape):

```json
{
  "schema": "camera-schedule.config.v1",
  "version": "0.7.0",
  "exported_at": "2026-05-31T22:14:07Z",
  "axparameters": {
    "lookahead_days": 7,
    "event_name_prefix": "",
    "poll_interval_seconds": 60
  },
  "anchors": [
    /* exact same shape as GET /anchors -> anchors[] (operator-defined only;
       built-ins are derived from settings/events.json on every boot
       and MUST NOT be exported). */
  ],
  "calendar": [
    /* exact same shape as GET /calendar -> entries[] */
  ],
  "schedule_enabled": {
    "sunrise": false,
    "moonrise": false
    /* full keyed map from localdata/schedule_enabled.json — only
       explicitly-toggled IDs appear; absence means default-enabled
       per FR-11.7 */
  },
  "debug_logging": false
}
```

The `schema` field MUST be `"camera-schedule.config.v1"` for v0.7.x
exports. Future bumps create v2, v3 etc.; v1 imports MUST keep
working forever (FR-12.4 forward compatibility).

### 1.4 `POST /local/camera_schedule/import`

**Auth**: `admin`-write.

Accepts the same envelope shape as `/export` produces. Validation
sequence:

1. Parse JSON. Malformed → HTTP 400 `{"error":"malformed_json"}`.
2. Validate `schema` field:
   - Missing or wrong → HTTP 400 `{"error":"schema_mismatch","message":"expected camera-schedule.config.v1"}`.
3. Validate each `anchors[]` entry against the same rules as POST /anchors.
   First failing entry → HTTP 400 with `{"error":"invalid_anchor","message":"<which one and why>"}`.
4. Validate each `calendar[]` entry against POST /calendar rules. First failing → HTTP 400 `{"error":"invalid_calendar",...}`.
5. Validate `schedule_enabled` map: keys are IDs (regex same as anchors), values are bool.
6. **Atomicity**: write all three target files (`anchors.json`,
   `calendar.json`, `schedule_enabled.json`) via the existing
   atomic-write helper (FR-12.1) **into a staging dir first**, then
   commit-rename them in a single transaction. If any step fails,
   the prior config is preserved unchanged.
7. On success: trigger an immediate recompute with trigger reason
   `import` and return the new `last_recompute` summary.

Pre-import safety: rename the existing config files to
`*.before-import-<timestamp>` and keep them on disk. Allows manual
recovery if a "successful" import turns out semantically wrong.
Garbage-collected after 7 days by the next import.

Success response:

```json
{
  "imported": {
    "anchors": 4,
    "calendar": 2,
    "schedule_enabled_keys": 6
  },
  "recompute": {
    "started_at": "2026-05-31T22:14:09Z",
    "trigger": "import",
    "elapsed_ms": 52,
    "events_armed": 23
  }
}
```

### 1.5 `GET /local/camera_schedule/debug`

**Auth**: `viewer`-readable. Returns the current debug-logging
state and the AXParameter scalar settings (read-only mirror; the
canonical writes go via `param.cgi`).

```json
{
  "debug_logging": false,
  "axparameters": {
    "lookahead_days": 7,
    "event_name_prefix": "",
    "poll_interval_seconds": 60
  }
}
```

### 1.6 `POST /local/camera_schedule/debug`

**Auth**: `admin`-write. Toggles the debug-logging state per
FR-13.4. Persisted via AXParameter (`DebugLogging`, "yes"/"no" or
"true"/"false" — pick one and document; cf. AXParameter integer
discipline below). Setting flushes immediately — no recompute
needed.

```http
POST /local/camera_schedule/debug
Content-Type: application/json

{"debug_logging": true}
```

Returns the updated state (same shape as GET).

---

## 2. AXParameter integration (FR-12.2)

This is **first-contact** for this codebase. Half a day of probe
before committing to interface shape — the SSE owns it.

### 2.1 Parameters to declare

The four AXParameters all live under the app's namespace
(`/usr/local/packages/camera_schedule/parameters.conf` or via
`AXParameter_Add` at boot — investigate which is the supported
path on AXIS OS 11.11+ and 12.x; both must work).

| Name | Type | Default | Range / format | Drives |
|---|---|---|---|---|
| `LookaheadDays` | int | 7 | 1..30 | how far ahead the recompute previews events_today (used by `events_today` endpoint already) |
| `EventNamePrefix` | string | "" | 0..32 chars, `[A-Za-z0-9 _-]*` | optional prefix prepended to the camera-side nice name when declaring AXEvent topics |
| `PollIntervalSeconds` | int | 60 | 30..600 | client status-panel poll rate hint (UI reads, server doesn't enforce) |
| `DebugLogging` | bool-as-string | "no" | "yes" / "no" | runtime LOG_DEBUG gate |

### 2.2 Dual write semantics

Per FR-12.2, AXParameters and the JSON status endpoint MUST stay in
sync. Sequence on `POST /debug`:
1. Validate body.
2. `AXParameter_Set("DebugLogging", "yes" or "no")`.
3. Flip the in-memory `g_debug_logging_enabled` flag.
4. Return current state.

On boot, read the AXParameter value and seed the in-memory flag.
**Same sequence applies if an external tool sets the AXParameter
via `param.cgi`** — register an AXParameter callback to re-read
and flip the in-memory flag.

### 2.3 LOG_DEBUG runtime gate

Currently the codebase has `LOG()`, `LOG_WARN()`, `LOG_ERROR()`
(after the M6 rename). Add `LOG_DBG()` macro:

```c
extern int g_debug_logging_enabled;
#define LOG_DBG(fmt, args...) do { \
    if (g_debug_logging_enabled) syslog(LOG_DEBUG, fmt, ## args); \
} while (0)
```

Defined where the other LOG macros live (currently scattered —
M7 work item: lift them all into a new `app/src/log.h` header
that all .c files include. SSE owns this refactor as part of M7.)

Then sweep existing `syslog(LOG_DEBUG, ...)` direct calls (if any)
to use `LOG_DBG`. Per FR-13.4 the toggle SHALL persist across
restarts but SHALL NOT be the default → AXParameter default "no".

---

## 3. status.c data model (SSE authors)

```c
// app/src/status.h
typedef enum {
    RECOMPUTE_TRIGGER_BOOT,
    RECOMPUTE_TRIGGER_MIDNIGHT,
    RECOMPUTE_TRIGGER_LOCATION_CHANGE,
    RECOMPUTE_TRIGGER_MANUAL,
    RECOMPUTE_TRIGGER_CONFIG_CHANGE,
    RECOMPUTE_TRIGGER_IMPORT,
    RECOMPUTE_TRIGGER_TZ_CHANGE
} recompute_trigger_t;

typedef struct {
    time_t started_at_utc;
    recompute_trigger_t trigger;
    int   elapsed_ms;
    int   anchors_evaluated;
    int   events_armed;
    int   skipped_polar;
    int   skipped_disabled;
    int   skipped_past;
    int   errors;
} recompute_summary_t;

void status_init(void);
void status_record(const recompute_summary_t* s);
const recompute_summary_t* status_last(void);     // NULL if none
const recompute_summary_t* status_recent(int*);    // pointer + count<=50
const char* status_trigger_str(recompute_trigger_t); // for JSON
void status_set_next(time_t scheduled_utc, recompute_trigger_t reason);
void status_get_next(time_t* out_utc, recompute_trigger_t* out_reason);
```

The ring buffer is a simple 50-slot circular array protected by
GMutex (the GLib main loop is single-threaded but AXParameter
callbacks may fire on a worker thread).

`timers.c` is updated to bracket each recompute with
`status_record()`. The bracketing replaces the existing FR-10.4
INFO syslog (or co-exists — keep the INFO log for journal
diagnostics and add the structured ring entry).

---

## 4. Manifest changes

Add four new entries to `manifest.json` httpConfig:

```json
{ "access": "viewer", "name": "status",     "type": "fastCgi" },
{ "access": "admin",  "name": "recompute",  "type": "fastCgi" },
{ "access": "admin",  "name": "export",     "type": "fastCgi" },
{ "access": "admin",  "name": "import",     "type": "fastCgi" },
{ "access": "viewer", "name": "debug",      "type": "fastCgi" }
```

Bump `version` to `"0.7.0"`. (Note: `debug` is viewer-readable for
the GET; the POST 403-rejects non-admin sessions — the framework's
RBAC enforces on the POST endpoint based on `access: viewer` is
read-only convention, BUT M6's `events` is `admin` and POSTs work
because the session must have the role. **SSE: confirm against M6
patterns.** If the framework requires `admin` to allow POST, declare
`debug` as `admin` and the GET will still work for admin sessions —
viewers won't see the debug toggle UI which is acceptable.)

---

## 5. UI surface (UI designer authors)

### 5.1 Status panel — top of `schedule.html`

Add a collapsible `<section>` above the existing search bar (or
below the `app-nav` block — designer's call). Default expanded.
Shows:

- **Last recompute**: relative time ("just now", "5 min ago"), trigger
  label, elapsed_ms, events_armed/anchors_evaluated count.
- **Next recompute**: relative time ("in 6 h 12 m") + trigger reason.
- **Location**: lat/lon, tz, valid/invalid badge.
- **Counts**: topics_declared, topics_enabled, anchors_user,
  calendar_entries.
- **Recent activity**: collapsible. Show last 10 of `recent[]` as
  a compact list (timestamp + trigger + elapsed_ms). "Show all 50"
  expands to full ring.

Polls `/status` every `axparameters.poll_interval_seconds` (default
60s). Visible-tab-only: pause polling when `document.hidden`.

### 5.2 Toolbar buttons

Add to the existing `.list-toolbar` row (next to "Refresh"):

- **Recompute now** — POSTs `/recompute`, transient "Recomputing…"
  spinner, success → flash the new last_recompute, error →
  `Failed — retry?` toast.
- **Export** — anchor-style link wired to `/export`. Browser
  handles the file-save dialog via Content-Disposition.
- **Import** — file picker (`<input type="file" accept="application/json">`)
  in a modal. On submit, POST the file body to `/import`; show
  per-error message from the response envelope; on success, flash
  "Imported N anchors / M calendar entries" and refresh the
  schedule list.

### 5.3 Debug-logging toggle

A separate small `<section>` near the bottom of `schedule.html`
(or in a new `settings.html` page — designer's call). Single
checkbox bound to `GET/POST /debug`. Save-on-change semantics
(no separate Save button), with the same Saving… / Failed
indicators used for FR-11.6 row toggles.

### 5.4 Accessibility audit (FR-11.4)

Sweep all 5 pages: `about.html`, `location.html`, `schedule.html`,
`anchors.html`, `calendar.html`. Checklist:

- Every interactive element keyboard-reachable (no `onclick` on
  div without `tabindex` + Enter/Space handler).
- Visible focus styles (not just :hover).
- Labeled form fields (`<label for=...>` or `aria-label`).
- ARIA roles on dynamic regions (`role="list"`, `aria-live`).
- Modal traps focus while open and restores it on close.
- Color-contrast meets WCAG 2.1 AA (4.5:1 body / 3:1 large text).
- Error messages announced to screen readers (`aria-live="polite"`
  on the relevant region).

Output a **concrete diff** to existing HTML/CSS/JS plus a
`docs/accessibility-audit-M7.md` checklist of what was checked
and the result. The STE produces a parallel audit checklist for
verification — coordinate via this contract.

---

## 6. Test surface (STE authors)

### 6.1 Host fixtures — export/import round-trip

Build `app/test/host/test_export_import.c`:

- Construct a synthetic config in-memory: 4 anchors (one of each
  kind), 2 calendar entries (one annual, one date-range), 6
  schedule_enabled toggles.
- Serialize via the same JSON path the `/export` endpoint will use
  (refactor the SSE's serializer into a callable function).
- Parse it back via the import validator and round-trip the result
  through serialize again — two outputs MUST be byte-identical
  (canonical key order required).
- Reject cases (each one separate test):
  - schema field missing.
  - schema field wrong (`camera-schedule.config.v0`,
    `camera-schedule.config.v2`).
  - anchor offset out of [-1440, 1440].
  - calendar entry with `kind: "single_date"` and missing `date`.
  - schedule_enabled key with regex-failing ID.
  - bool fields with non-bool values.
- Atomicity: simulate failure mid-write (the test injects an
  EIO into the helper) and verify the prior config is intact.

### 6.2 Soak harness

Build `app/test/lab/soak_24h.sh`:

- Reads `AXIS_HOST_OS12` and `AXIS_PASS` from env (memory file).
- Every 60 s for 24 h, sample:
  - `RSS` of `camera_schedule` process (via `top -b -n 1` in an
    `axis-cgi/admin/dist1.cgi` shell wrapper, or via SSH if
    enabled — investigate; M7 work item is to **document the
    chosen mechanism** in `docs/soak.md`).
  - `apparmor_status` / journal.
  - `GET /status` response (full body, persisted as ND-JSON to
    `soak/<startdate>.ndjson`).
- Failure conditions (any one fails the run):
  - RSS grows > 20% from minute-5 baseline over the 24 h window.
  - Any AppArmor DENIED entry referencing `camera_schedule`.
  - `GET /status` non-200 for > 3 consecutive samples.
  - `last_recompute.errors` ever > 0.
- Output a summary report to `soak/<startdate>-report.md`.

Also build a **1-hour pilot** (`soak_1h.sh` — same harness, shorter
window) that we run before tagging v0.7.0; the 24-h run is a
gating step but tag is allowed to ship before it completes (per
the lessons file: M7 is calendar-locked nowhere except the soak
itself).

### 6.3 Accessibility verification

Pure review work. STE produces
`docs/accessibility-audit-M7-stetest.md` with the same checklist
items applied independently to the UI agent's output. Discrepancies
get raised before integration.

---

## 7. File ownership / who-touches-what

To minimize merge conflicts in worktrees, agents own files
exclusively unless flagged as **shared**.

**SSE owns:**
- `app/src/status.h` (new)
- `app/src/status.c` (new)
- `app/src/log.h` (new — lift LOG/LOG_WARN/LOG_ERROR/LOG_DBG)
- `app/src/main.c` — adds 5 endpoint handlers, AXParameter init,
  shared with UI integrator only on the Makefile if anything new
  needs OBJS-listing; UI does not edit main.c.
- `app/src/timers.c` — instrument with status_record() bracketing.
- `app/Makefile` — add status.o to OBJS, add test-export-import target.
- `app/manifest.json` — add 4 httpConfig entries, bump version.

**UI owns:**
- `app/html/schedule.html` — add status panel + toolbar buttons.
- `app/html/js/schedule.js` — extend with status polling + button
  handlers + import-modal.
- `app/html/js/import.js` (new — if the import modal lives in its
  own file).
- `app/html/css/app.css` — extend.
- `app/html/about.html`, `location.html`, `anchors.html`,
  `calendar.html` and their JS — a11y patches only (no behavior
  changes).
- `docs/accessibility-audit-M7.md` (new).

**STE owns:**
- `app/test/host/test_export_import.c` (new).
- `app/test/lab/soak_24h.sh` (new).
- `app/test/lab/soak_1h.sh` (new).
- `docs/soak.md` (new).
- `docs/accessibility-audit-M7-stetest.md` (new).

**Shared (touched by integrator at merge time only):**
- `app/Makefile` — STE adds `test-export-import` target; SSE
  adds OBJS entry. Both edits are append-only and trivially
  mergeable.
- `CLAUDE.md` — updated by the integrator at tag time, not by
  any agent.

---

## 8. AXEvent topic side-effects

M7 adds **no new event topics**. The 22 built-ins + operator
anchors + calendar topics from M6 remain untouched. The
`EventNamePrefix` AXParameter, when set, applies on next boot
(simpler than reconciling existing declarations). Document this:
operators changing the prefix must restart the app for it to
take effect.

---

## 9. Backward compatibility

- Existing endpoints (`about`, `location`, `anchors`, `calendar`,
  `events`, `events_today`) remain unchanged at the wire level.
- Existing `localdata/*.json` files remain unchanged on disk.
- A v0.6.0 → v0.7.0 in-place upgrade (existing operator config
  preserved) is mandatory and tested.

---

## 10. Lessons-encoded policies (read before starting)

1. **Worktree commits are mandatory.** Each agent **MUST** `git
   add && git commit` their work before declaring done. Worktrees
   that contain no commits are auto-cleaned by the harness (this
   bit us in M6). Use a feature branch: `m7/<role>` (`m7/sse`,
   `m7/ui`, `m7/test`).

2. **Branch from current `main` (HEAD must include v0.6.0).**
   Confirm `git log -1` shows `f0078a4` or later before starting.
   M6 had three worktrees branched from a stale parent; they
   couldn't see Phase 1 outputs.

3. **Use `ACAP_HTTP_Respond_JSON` for all JSON responses.** Never
   `Respond_String`. The 4096-byte cap is real. `/status` ring
   buffer at 50 entries × ~250 B = 12.5 KB — well past the cap if
   anyone is tempted to format with `%s`.

4. **Add new files to `acap-build`'s `-a` list in `app/Dockerfile`
   if they're NOT under `html/` or `lib/`.** M7 may not need any
   (no new bundled config) but if you add anything, verify with
   `tar tzf dist/camera-schedule-armv7hf.eap | sort` post-build.

5. **AXParameter is first-contact.** SSE: spike the API against
   the OS 12 lab camera (`10.1.40.113`) before committing. If a
   capability turns out to be wrong on shipping firmware, append
   an OQ-14 entry and resolve via DL-20 in
   `requirements/24-open-questions.md` and
   `requirements/28-decision-log.md` respectively.

6. **No backwards-compatibility hacks.** Per the project rules
   in CLAUDE.md, when changing existing code (e.g. lifting LOG
   macros into `log.h`), update all call sites — don't
   re-export from old locations.

7. **No comments restating the obvious.** Per project conventions,
   only document the non-obvious WHY.

8. **The accessibility audit is NOT a tickbox.** UI is expected
   to deliver real keyboard navigation, real focus states, real
   ARIA. STE audits independently.

---

## 11. Acceptance gate (M7 done-definition)

Tag `v0.7.0` when:

- Both `.eap` artifacts build green in CI.
- Both lab cameras show:
  - All 5 new endpoints responding correctly (round-trip via
    `curl`).
  - Status panel renders in the camera UI's Apps tab; numbers
    update on `Recompute now`.
  - Export downloads a valid envelope; import of a synthetic
    config replaces config and triggers a recompute.
  - Debug toggle persists across restart.
  - AXParameters visible in the camera's standard parameter UI
    and via `param.cgi`.
- Host fixtures (`make test`) all pass including new
  `test-export-import` target.
- 1-hour soak pilot completes clean.
- `docs/accessibility-audit-M7.md` and
  `docs/accessibility-audit-M7-stetest.md` reconciled.
- CLAUDE.md "Repository state" updated.

The 24-hour soak is a **post-tag gating step** — it begins at
tag time. M7 is "fully done" when the 24-hour soak report is
clean and committed.
