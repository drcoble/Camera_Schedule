# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository state

**M0–M3 complete.** Tagged releases: `v0.1.0` (M1 build pipeline +
empty `.eap`), `v0.2.0` (M2 sunrise/sunset MVP), `v0.3.0` (M3 full
solar suite + polar safety).

The current application boots a GLib main loop, exposes
`about` / `location` FastCGI endpoints, declares 10 AXEvent topics
covering the full FR-3 solar suite (sunrise/sunset, civil/nautical/
astronomical twilight pairs, solar noon, solar midnight), and arms
per-event GLib timers from a Meeus-based solar-position routine. All
10 topics are bound to operator action rules in the lab cameras'
Action Rules UI; polar latitudes correctly produce SOLAR_NO_EVENT
INFO logs for sunrise/sunset while keeping solar noon/midnight armed.
Worst host-fixture error is 40 s of FR-3.7's 60 s tier-1 budget.

Next milestone is **M4 — Lunar events** per
[`IMPLEMENTATION.md`](./IMPLEMENTATION.md). M4 introduces a new
`app/src/astro/lunar.c|h` for moonrise / moonset / lunar transit /
phases (Meeus chapters 47 + 49) and adds 8 new entries to
`settings/events.json`. Don't take on M5+ work until M4's lab-camera
gate (`fullmoon` action rule fires on the next published full-moon
date) is verified.

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
