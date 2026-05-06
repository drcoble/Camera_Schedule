# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository state

**M0–M3 shipped.** Tagged releases: `v0.1.0` (M1 build pipeline +
empty `.eap`), `v0.2.0` (M2 sunrise/sunset MVP), `v0.3.0` (M3 full
solar suite + polar safety).

**M6 shipped** at `v0.6.0`: operator-defined anchors (offset /
paired / numeric-threshold), user calendar entries (single-date /
date-range / annual), the unified Schedule list view (FR-11.6),
and the firing-path-only enable/disable gate (FR-11.7 + DL-18).
Running on both lab cameras with the four new FastCGI endpoints
(`anchors`, `calendar`, `events`, `events_today`) verified
end-to-end via VAPIX round-trip.

**M7 shipped** at `v0.7.0`: status panel (FR-11.2), 50-entry
in-memory recompute ring buffer (FR-13.3), `LOG_DBG` runtime gate
toggled via the persistent debug-logging AXParameter (FR-13.4),
config export/import with `camera-schedule.config.v1` envelope and
schema validation (FR-12.3), AXParameter exposure of four scalar
settings under `root.Camera_schedule.*` (FR-12.2), "Recompute now"
button (FR-10.2), and the formal accessibility audit across all
five HTML pages (FR-11.4). Five new FastCGI endpoints — `state`,
`recompute`, `export`, `import`, `debug` — lab-verified end-to-end
via VAPIX round-trip on both cameras. Note the contract endpoint
was originally `/status` but the vendored Timelapse2 framework
already registers a `/status` node at boot (`ACAP.c:691`); a silent
duplicate-path warning means our handler never reaches the wire.
Renamed to `/state` per DL-22; OQ-16 captured the trap.

**M4 + M5 code-complete and lab-installed; calendar-locked tags
still pending.** The M4/M5 implementations ship inside v0.6.0 and
are running on both cameras (10.1.40.113 / OS 12.10.61,
10.1.40.160 / OS 11.11.192). Tags `v0.4.0` and `v0.5.0` are still
held until the firing gates verify on their respective dates —
v0.6.0 was tagged ahead of them because its acceptance gate is
operator UI + CRUD verification, not a calendar event:

- **M4 fullmoon gate**: 2026-05-31 (next full moon). Verifies the
  lunar-phase scheduler in `timers.c` correctly re-arms after fire.
- **M5 junesolstice gate**: 2026-06-21 08:24 UTC. Verifies the
  seasonal scheduler and that the `Longest Day` hemisphere-aware
  label renders correctly in the Action Rules UI for Northern lat.

Verifying those gates retroactively tags v0.4.0 and v0.5.0; v0.6.0
stays as the rolling head since it includes both M4 and M5 code.

The current application boots a GLib main loop, exposes eleven
FastCGI endpoints (`about`, `location`, `anchors`, `calendar`,
`events`, `events_today`, `state`, `recompute`, `export`, `import`,
`debug`), exposes four AXParameters (`LookaheadDays`,
`EventNamePrefix`, `PollIntervalSeconds`, `DebugLogging`), and
declares **22 built-in AXEvent topics** plus operator-defined
topics added at runtime via `ACAP_EVENTS_Add_Event`:

- 10 solar (FR-3): sunrise/sunset, civil/nautical/astronomical
  twilight pairs, solar noon, solar midnight (Meeus solar position).
- 8 lunar (FR-4): moonrise/moonset/transit/anti-transit (daily,
  parallax-aware via Meeus ch. 47 hourly altitude sampling) +
  newmoon/firstquarter/fullmoon/lastquarter (Meeus ch. 49 phases).
- 4 seasonal (FR-5): marchequinox/junesolstice/septemberequinox/
  decembersolstice (Meeus ch. 27 closed-form + Table 27.C corrections
  + Espenak-Meeus ΔT, see DL-19).

Operator anchors persist as JSON in `localdata/anchors.json`,
calendar entries in `localdata/calendar.json`, and the FR-11.7
enable map in `localdata/schedule_enabled.json` — all written
through the new `app/src/persistence.c` atomic-write helper
(write-temp + fsync + parse-back + schema-validate + rename per
FR-12.1).

Four scheduler patterns coexist in `timers.c`:

- **Daily slots** (10 solar + 4 lunar daily) — recomputed at local
  civil midnight, armed for the new UTC date.
- **Phase slots** (4 lunar phases) — observer-independent, ~29.5 d
  cadence, re-arms in own fire callback. Armed once at boot.
- **Season slots** (4 seasonal) — observer-independent, ~91 d
  cadence, re-arms in own fire callback. Armed once at boot.
- **Anchor slots** (M6) — dynamic, rebuilt per recompute from
  `anchors_get_by_index`. Pulse / stateful-offset / paired-interval
  / numeric-threshold variants. Threshold anchors fire at local
  civil midnight on satisfying days (proxy for solar midnight per
  OQ-13).

Phase, season, and threshold-day slots are NOT touched by
`timers_recompute_now()` for non-content changes that don't move
the satisfying-day set. Hemisphere-aware solstice labels (FR-5.2)
are rebound by `apply_seasonal_labels()` in main.c at boot and on
every location POST.

The FR-11.7 firing-path enable gate (`if
(!anchors_is_enabled(slot->event_id)) continue;`) is at the top
of `arm_event_slot`, `arm_phase_slot`, `arm_season_slot`, and the
anchor-slot arm helpers. Disabled topics stay declared per DL-18 —
only firing is suppressed.

Polar latitudes still produce SOLAR_NO_EVENT / LUNAR_NO_EVENT INFO
logs for rise/set while keeping the noon/midnight/transit slots
armed. Host-fixture worst errors: solar 40s of FR-3.7's 60s tier-1
budget; lunar 22s of FR-4.5's 120s budget for habitable rise/set;
seasonal 60s on a single 2028 boundary fixture, all others ≤50s.

The M7 status ring buffer (`app/src/status.{c,h}`, 50 entries,
GMutex-protected) records every recompute scope from
`timers_recompute_now()`. `GET /state` returns the live snapshot
plus `rss_kb` from `/proc/self/status` (DL-21). The debug-logging
flag drives the `LOG_DBG` macro in the new shared `app/src/log.h`
(which also lifted `LOG`, `LOG_WARN`, `LOG_ERROR` from main.c so
all .c files can include one header). The export/import envelope
shape is documented in `app/src/M7_API_CONTRACT.md §1.3-1.4`;
host fixture `test_export_import.c` exercises round-trip identity
plus 10 reject cases (12/12 pass).

Next milestone is **M8 — Beta release readiness** per
[`IMPLEMENTATION.md`](./IMPLEMENTATION.md), tagged as `v1.0.0-beta`
per [DL-23](./requirements/28-decision-log.md). M8 scope is
license-audit CI gate (NFR-6), reproducible-build verification (BR-6),
`CONTRIBUTING.md`/`SECURITY.md`/`CHANGELOG.md` polish, and a public
release page with **unsigned** `.eap` files for both armv7hf and
aarch64. Application Signing (formerly DL-11) and Artpec-8+ hardware
verification are deferred — both are gates on promoting from
`v1.0.0-beta` to `v1.0.0` GA, not on M8 itself. Don't tag v0.4.0 or
v0.5.0 until both calendar gates above have produced observable fires
from bound action rules. The 24-hour soak harness
(`app/test/lab/soak_24h.sh`) is the post-M7-tag gating step that
samples `/state.rss_kb`, AppArmor `system_log.cgi`, and recompute
errors every 60 s for 24 h.

## Where to read first

In order:

1. [`README.md`](./README.md) — one-page overview, scope, and the index
   to every requirements file.
2. [`IMPLEMENTATION.md`](./IMPLEMENTATION.md) — milestone-driven
   roadmap (M0…M8). Tells you which milestone is next and what
   "complete" means for it. Treat the milestone you're working on as
   the active task list; don't take on M+1 work until M's tests pass.
3. [`requirements/28-decision-log.md`](./requirements/28-decision-log.md)
   — append-only ledger of every substantive design decision. **Read
   this before quoting any specific requirement** — DL-NN entries
   document architectural pivots and removed requirements. The decision
   log is the canonical "current state of thinking."
4. [`requirements/24-open-questions.md`](./requirements/24-open-questions.md)
   — closed-OQ ledger. All OQ-1…OQ-12 are resolved. New uncertainties
   get appended here as **OQ-13+** with a corresponding **DL-19+**
   resolution. (DL-17 captured the lat/lon UI-rounding rule, DL-18 the
   schedule enable/disable model — neither was triggered by an OQ.)
5. The numbered requirements files. Functional requirements are in
   `01-…` through `13-…`; cross-cutting (non-functional, platform,
   build, verification, licensing, environment, framework reuse,
   decisions) in `20-…` through `28-…`.

## Working conventions

### Decision log discipline

- The decision log is **append-only**. Once a DL-NN entry is recorded,
  do not edit it. To reverse or refine a prior decision, append a new
  DL entry that explicitly says "supersedes DL-NN" and update only the
  earlier entry's status to `superseded by DL-NN`.
- Whenever you remove or materially change a requirement, capture the
  removal in a new DL entry. The "Removed / changed" block is what
  future contributors will rely on to understand why something looks
  the way it does.

### Historical artifacts

Two files are historical snapshots and SHOULD NOT be retroactively
edited (they document state at a specific point in time):

- [`requirements/26-discovered-environment.md`](./requirements/26-discovered-environment.md)
  — the result of the initial probe sweep against the lab cameras.
  Firmware versions and observed behaviors are correct *as of the probe
  date*. If a camera is upgraded or behavior changes, capture that in a
  new DL entry, not by editing this file.
- The decision log itself.

### Open questions hygiene

The OQ file is the question ledger, not a design document. Don't copy
rationale into it; rationale belongs in the corresponding DL entry. An
OQ entry should be one sentence + a pointer to its resolution.

### Cross-references

Use relative paths within the `requirements/` tree
(`./07-schedule-anchors.md`, `./28-decision-log.md`). Reference specific
clauses by their identifier (`FR-7.4`, `NFR-6`, `DR-1`, `BR-7`,
`DL-05`). When you remove a file, sweep cross-refs:

```sh
grep -rn 'old-filename\|removed-id' /Users/drew/Documents/Development/Camera_Schedule/
```

### Trust-but-verify when recalling

A few earlier draft requirements were proven wrong by hardware probing
(e.g. the assumption that `/config/rest/scheduled-events/v2` exists, or
that AXIS OS 12 is aarch64-only). Before recommending an approach from
public Axis documentation, **check the decision log first** to see if
the assumption was already invalidated. The lab cameras (see below) are
the source of truth; the docs at `developer.axis.com` describe ideal
state, not all shipping firmware.

## Architectural commitments (don't relitigate without a DL entry)

These are decided. If you find a reason to reverse one, write a new
DL-NN entry first.

- **License**: MIT. Single `LICENSE` file at root, SPDX header in every
  project-owned source file. (DL-01)
- **Open source**: public repo, public CI, public release artifacts.
  No credentials in tree. (DL-02, DL-09)
- **Schedule mechanism**: **Path A — publish ACAP event topics**.
  The app registers events on the device event engine and fires them
  via GLib timer sources. **No iCalendar generation. No REST writes to
  a schedule API.** That API doesn't exist on shipping firmware.
  (DL-05; see `08-event-registration.md`, `09-event-firing.md`.)
- **Framework reuse**: vendor `ACAP.c/h` and `cJSON.c/h` from
  Timelapse2 (MIT, https://github.com/pandosme/Timelapse2) into
  `app/src/acap/`. Adopt its declarative-events pattern
  (`settings/events.json`) and its midnight + per-event timer model.
  (DL-06; see `27-reuse-from-timelapse2.md`.)
- **Geolocation**: lat/lon lives in the camera's geolocation service.
  Manual overrides write back via `/axis-cgi/geolocation/set.cgi`. The
  app does not maintain a parallel persistent override. (DL-07)
- **No external CDN** in the UI: bundle all assets locally.
  Specifically diverges from Timelapse2's Leaflet-from-unpkg pattern.
  (DL-08)
- **OS floor**: AXIS OS **11.11+**. Single mainline. Two `.eap`
  artifacts (armv7hf, aarch64). One manifest schema 1.7.x. One SDK
  12.x. (DL-15)
- **No `compatibleOsVersions` in manifest**: that field doesn't exist
  in any schema bundled with SDK 12.6.0 (verified v1.0…v1.8.0). OS
  compatibility is enforced at runtime by the APIs we link against.
  (DL-16)

## Build & test

The build is fully Dockerized — the only host-side requirement is
Docker (Buildx-capable) and `make`. Everything else (toolchain, SDK,
manifest validator, `acap-build` packaging) runs inside the pinned
`axisecp/acap-native-sdk:12.6.0-${ARCH}-ubuntu24.04` image.

### Local builds

From the repo root:

```sh
make -C app build-armv7hf   # OS 11.11+ and OS 12 on Artpec-7
make -C app build-aarch64   # OS 12 on Artpec-8+
make -C app build-all       # both
make -C app help            # menu
```

Outputs:

```
dist/camera-schedule-armv7hf.eap
dist/camera-schedule-aarch64.eap
```

The Makefile's `stage-license` prerequisite copies the root `LICENSE`
into `app/LICENSE` (gitignored) so `acap-build` can find it without
having two LICENSE files in git.

### Adding files to the .eap

`acap-build` only auto-bundles a fixed set of well-known directories
(`html/`, `lib/`) plus standard files (`LICENSE`, `manifest.json`,
the compiled binary). **Anything else is silently dropped** from the
produced `.eap` — no warning, no exit-code change. The first sign of
the bug is usually a runtime symptom (an `ACAP_FILE_Read(...)`
returning NULL, an asset 404 from the web UI, etc.).

When you add any other file under `app/` that needs to ship — config
under `settings/`, models, blob assets, anything outside `html/` —
add a `-a path/to/file` argument to the `acap-build` invocation in
`app/Dockerfile`:

```Dockerfile
RUN . /opt/axis/acapsdk/environment-setup-* \
    && acap-build . -a settings/events.json -a path/to/new/file
```

Always verify the bundle contents after a change:

```sh
tar tzf dist/camera-schedule-armv7hf.eap | sort
```

This trap cost us the M2 lab gate — `settings/events.json` was in the
build context but not in the `.eap`, so zero AXEvent topics declared
and the camera's Action Rules picker stayed empty. Fixed in commit
`42e671b`.

### CI artifacts

Every push and PR runs the full matrix (`build × {armv7hf, aarch64}` +
manifest sanity + `make help`). To grab a green run's `.eap` files
without rebuilding locally:

```sh
RUN_ID=$(gh run list --limit 1 --repo drcoble/Camera_Schedule \
           --status success --json databaseId --jq '.[0].databaseId')
gh run download $RUN_ID --repo drcoble/Camera_Schedule --dir dist
```

### Naming conventions

- **`appName`** = `camera_schedule` (underscore). Drives the URL path
  `/local/camera_schedule/...` and the AXEvent topic prefix
  `tnsaxis:CameraApplicationPlatform/camera_schedule/...`.
- **Artifact filenames** use a hyphen: `camera-schedule-<arch>.eap`.
- **`friendlyName`** is `Camera Schedule` (space) — that's what
  appears in the camera's Apps UI.

### Installing on a lab camera (VAPIX, no SDK tools required)

```sh
export AXIS_HOST_OS12=...   # from memory/test_cameras.md
export AXIS_HOST_OS11=...
export AXIS_PASS=...

EAP=dist/camera-schedule-armv7hf.eap

# Upload (works for both first-install and reinstall)
curl -sk --anyauth -u "root:$AXIS_PASS" \
  -F "packfil=@${EAP}" \
  "https://${AXIS_HOST_OS12}/axis-cgi/applications/upload.cgi"

# Start
curl -sk --anyauth -u "root:$AXIS_PASS" \
  "https://${AXIS_HOST_OS12}/axis-cgi/applications/control.cgi?action=start&package=camera_schedule"
```

### M1 smoke check

```sh
curl -sk --anyauth -u "root:$AXIS_PASS" \
  "https://${AXIS_HOST_OS12}/local/camera_schedule/about"
# {"name":"Camera_Schedule","version":"0.1.0","arch":"armv7hf"}
```

## Test cameras

Two lab Axis cameras exist on the development network. **Their IPs and
root credentials live in the developer's local Claude memory at**
`/Users/drew/.claude/projects/-Users-drew-Documents-Development-Camera-Schedule/memory/test_cameras.md`,
**not in this repo.** They MUST stay out of the repo because the repo
is public-MIT.

Probe scripts and (eventually) CI integration tests SHALL read camera
addresses and credentials from environment variables — typical pattern:

```sh
export AXIS_HOST_OS12=...   # from memory file
export AXIS_HOST_OS11=...
export AXIS_PASS=...

curl -sk --anyauth -u "root:$AXIS_PASS" \
  "https://$AXIS_HOST_OS12/axis-cgi/geolocation/get.cgi"
```

Both cameras are Artpec-7 / armv7hf. **The aarch64 build artifact has
no lab target yet** — an Artpec-8+ unit is needed before a v1 release
can ship.

### Probing

All probes against the cameras MUST be **read-only by default**. Use
`GET /axis-cgi/...` and SOAP read operations only. Anything that
mutates camera state (write a parameter, push a config, install an
ACAP) requires user confirmation first — these are shared lab cameras,
not dedicated to one experiment.

## File numbering scheme

```
01-13   Functional requirements (FR-1 through FR-13)
20      Non-functional requirements (NFR)
21      Platform & compatibility (PR)
22      Build & packaging (BR)
23      Verification (acceptance checks)
24      Open-questions ledger (closed; new ones append as OQ-12+)
25      Licensing & distribution (DR)
26      Discovered environment (probe-time historical)
27      Reuse from Timelapse2
28      Decision log (DL, append-only)
```

Don't renumber. If a new cross-cutting topic emerges that doesn't fit,
add it as the next available 2x file.

## Memory pointer

This project has its own memory tree at
`/Users/drew/.claude/projects/-Users-drew-Documents-Development-Camera-Schedule/memory/`.
Currently it contains:

- `test_cameras.md` — lab camera addresses, credentials, and upgrade
  history.
- `github_handle.md` — the user's GitHub handle is `drcoble` (not
  `drewcoble`); repo is `drcoble/Camera_Schedule`.

Add new memory files there for any project-specific context that
shouldn't enter the public repo.
