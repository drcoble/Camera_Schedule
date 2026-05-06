# Implementation plan

A milestone-driven roadmap for taking Camera_Schedule from "requirements
only" to a tagged v1.0 release. Each milestone is **independently
shippable**: ends with a tagged release, an installable `.eap`, and an
operator-visible improvement. Each milestone has its own tests; nothing
moves to the next milestone until the current one's tests pass on both
lab cameras.

The architecture is fixed by the [requirements](./requirements/) — read
[`requirements/28-decision-log.md`](./requirements/28-decision-log.md)
first. This file only sequences the work.

---

## Working loop (every milestone)

1. Implement the feature.
2. Add host-side unit tests for any new astronomy or anchor logic.
3. Build both `.eap` artifacts in CI.
4. Install on both lab cameras (OS 12.10.61 and OS 11.11.192).
5. Verify the operator-visible behavior in the camera's Action Rules UI.
6. Update [`requirements/23-verification.md`](./requirements/23-verification.md)
   with any newly satisfied acceptance check.
7. Tag a release. Move on.

If a milestone surfaces a new uncertainty, append it as **OQ-12+** in
[`requirements/24-open-questions.md`](./requirements/24-open-questions.md)
and resolve via a new **DL-NN** decision-log entry before continuing.

---

## M0 — Repo foundation (no shippable artifact)

**Goal.** Get the repo to a state where `git clone` followed by
"open in a C IDE" makes sense.

**Deliverables.**

- `git init`, first commit with current `requirements/`, `README.md`,
  `CLAUDE.md`, this file.
- `LICENSE` (full MIT text), `.gitignore`, `CONTRIBUTING.md`,
  `SECURITY.md`, `CODE_OF_CONDUCT.md`, `THIRD_PARTY_LICENSES.md`
  (initially empty placeholder).
- Empty directory tree under `app/` matching the layout in
  [BR-7 source layout](./requirements/22-build-and-packaging.md).
- GitHub repo created public; protected `main` branch; placeholder CI
  workflow that runs `make help`.

**Tests.** None — structure only.

**Exit.** `git push origin main` succeeds; the repo renders cleanly on
GitHub; CI runs and passes.

---

## M1 — Build pipeline + empty .eap (release v0.1.0)

**Goal.** Prove the build matrix and framework vendoring work end-to-end
before writing any business logic.

**Deliverables.**

- `app/manifest.json` — schemaVersion 1.7.x, `embeddedSdkVersion: "3.0"`,
  `runMode: "respawn"`, `compatibleOsVersions` set per
  [PR-1](./requirements/21-platform-compatibility.md), and an
  `httpConfig` entry for a single FastCGI endpoint named `about`.
  D-Bus declares `VAPIXServiceAccounts1.GetCredentials`.
- `app/Dockerfile` — pinned `axisecp/acap-native-sdk:12.x`, parameterized
  by `--build-arg ARCH=`.
- `app/Makefile` — `build-armv7hf`, `build-aarch64`, `build-all`, plus
  `clean`.
- Vendored from Timelapse2 (MIT, attribution per
  [DR-10](./requirements/25-licensing-and-distribution.md)):
  - `app/src/acap/ACAP.c`, `ACAP.h`, `cJSON.c`, `cJSON.h`.
- `app/src/main.c` — boots GLib main loop, calls `ACAP_HTTP_Node("about", …)`,
  registers no events.
- `app/html/about.html` + minimal `js/app.js` that GETs `about` and
  renders version + arch.
- `THIRD_PARTY_LICENSES.md` lists the Timelapse2 file lift.
- CI workflow that builds both `.eap` files and uploads them as
  artifacts.

**Tests.**

- `make build-all` produces two `.eap` files, each ≤ 5 MB.
- `eap-install.sh install camera-schedule-armv7hf.eap` succeeds on both
  lab cameras (OS 12.10.61 and OS 11.11.192).
- After start, `https://<camera>/local/camera_schedule/about` returns
  `{"name":"Camera_Schedule","version":"0.1.0","arch":"armv7hf"}`.
  (URL path uses an underscore — Axis manifest schemas restrict
  `appName` to `[a-zA-Z0-9_]`, so `camera_schedule` is the appName and
  the `/local/` path is derived from it. Hyphens stay in the artifact
  filenames only.)
- `apparmor_status` shows no policy violations on the OS 12 camera over
  a 1-hour run.

**Exit.** Tag `v0.1.0`. Update CLAUDE.md "Build & test" section with the
real `make` targets.

**Why first?** Every later milestone is "add a file or two and rebuild."
Getting the pipeline right is the highest-risk, lowest-fun piece — front-load it.

---

## M2 — Sunrise / sunset MVP (release v0.2.0)

**Goal.** Operator can install, configure lat/lon, and bind a camera
action rule to a working sunrise or sunset trigger.

**Deliverables.**

- `app/src/astro/solar.c|h` — solar-position routine refactored from
  Timelapse2's `sunevents.c`, parameterized by zenith angle. Supports
  the standard sunrise/sunset zenith **90.833°** for now.
- `app/settings/events.json` — two entries: `sunrise`, `sunset`.
- `app/src/timers.c|h` — midnight scheduler (`g_timeout_source_new_seconds`)
  recomputes daily; per-event one-shot timers fire each topic via
  `ACAP_EVENTS_Fire(...)`.
- `app/src/main.c` — wires up: read camera lat/lon via
  `ACAP_DEVICE_Latitude/Longitude`, register events, arm timers.
- Minimal location UI: `app/html/location.html` shows current lat/lon
  with a numeric form. POST to a new `location` FastCGI endpoint that
  calls `ACAP_DEVICE_Set_Location` (no map; matches
  [DL-08](./requirements/28-decision-log.md)).
- `app/test/host/test_solar.c` — host-side fixture comparing computed
  sunrise/sunset against USNO data for ≥ 10 lat/lon × date pairs.

**Tests.**

- `make test` (host) passes the solar fixtures within ±60 s
  ([FR-3.7](./requirements/03-solar-events.md)) at lat ≤ ±60°.
- On the OS 12 lab camera with location set to Atlanta GA: the topics
  `tnsaxis:CameraApplicationPlatform/camera_schedule/sunrise` and
  `…/sunset` appear in the Action Rules condition picker, with their
  configured nice names.
- Bind an action rule "sunrise → send MQTT message" and observe it fire
  within ±2 min of the USNO sunrise for that date.
- After the camera's wall clock crosses solar midnight, fresh
  per-event timers are armed for the new day.

**Exit.** Tag `v0.2.0`.

---

## M3 — Full solar suite + polar safety (release v0.3.0)

**Goal.** Every solar event in [FR-3](./requirements/03-solar-events.md)
fires correctly, including in polar regions.

**Deliverables.**

- `solar.c` extended to support all six twilight zeniths (90.833°, 96°,
  102°, 108°) plus solar noon (transit) and solar midnight
  (anti-transit).
- `solar.c` returns a `NO_EVENT_TODAY` sentinel when `acos` would
  overflow (polar night/day); the caller skips arming that day's
  timer and emits an INFO log.
- `events.json` extended to all 10 solar event ids.
- Host-side fixtures expanded to include latitudes 70°N and 78°N
  across the winter solstice (verifies polar branches), and DST
  spring-forward / fall-back boundaries.

**Tests.**

- All host fixtures pass.
- On the lab camera, all 10 solar topics appear in the Action Rules UI.
- Configuring the override lat/lon to **78°N**: the app starts cleanly,
  logs "no event today" for sunrise/sunset on appropriate winter days,
  and still fires solar midnight.

**Exit.** Tag `v0.3.0`.

---

## M4 — Lunar events (release v0.4.0)

**Goal.** Moon phase, illumination, transit, and rise/set events fire
correctly.

**Deliverables.**

- `app/src/astro/lunar.c|h` — Meeus *Astronomical Algorithms* ch. 47
  (lunar position) and ch. 49 (phases). Parallax-aware moonrise/moonset
  via hourly altitude sampling + linear interpolation.
- 8 new entries in `events.json`: `moonrise`, `moonset`, `moonnoon`,
  `moonmidnight`, `newmoon`, `firstquarter`, `fullmoon`, `lastquarter`.
- Illumination fraction exposed as a numeric value via the future
  status endpoint (consumed in M6 by numeric-threshold anchors).
- Host fixtures: ≥ 1 full Metonic-cycle of new/full moons; moonrise/set
  for ≥ 5 lat/lon × date pairs against published almanac data.

**Tests.**

- Host fixtures pass within
  [FR-4.5](./requirements/04-lunar-events.md) tolerances (≤ 2 min for
  rise/set/transit; ≤ 5 min for phases; ≤ 2pp for illumination).
- On the lab camera: bind an action rule to `fullmoon` and verify it
  fires on the next published full-moon date.

**Exit.** Tag `v0.4.0`.

---

## M5 — Seasonal events (release v0.5.0)

**Goal.** Solstices and equinoxes fire correctly, with hemisphere-aware
"Longest Day" / "Shortest Day" labels.

**Deliverables.**

- `app/src/astro/seasonal.c|h` — Meeus ch. 27 closed-form
  approximations.
- 4 new entries in `events.json`. Nice names are determined at boot
  from the sign of `Latitude`:
  - lat ≥ 0.5°: June solstice → "Longest Day", December → "Shortest Day"
  - lat ≤ −0.5°: inverse
  - |lat| < 0.5°: neutral "June solstice", "December solstice"
- Host fixtures for solstice/equinox dates and times for 2026–2030.

**Tests.**

- Host fixtures pass within ±60 s.
- On the lab camera: nice names render correctly for Atlanta (Northern
  Hemisphere) and for a configured override at lat=−34° (Sydney
  approximation).

**Exit.** Tag `v0.5.0`.

---

## M6 — Anchors, offsets, and user calendar (release v0.6.0)

**Goal.** Operators can define their own anchors with offsets,
durations, paired-event intervals, and numeric thresholds; plus
arbitrary calendar dates.

**Deliverables.**

- `app/src/anchors.c|h` — anchor data model and topic-id derivation.
  Persisted as JSON in `localdata/`. Edits trigger
  [FR-8.5](./requirements/08-event-registration.md) reconciliation.
- Offset arithmetic, duration handling, paired-anchor stateful events,
  numeric-threshold anchors (uses M4's illumination fraction).
- `app/src/calendar.c|h` — single-date, date-range, annual-recurring
  entries. Each becomes its own event topic.
- New FastCGI endpoints: `anchors` (GET/POST), `calendar` (GET/POST),
  `events_today` (GET — preview).
- HTML UI pages for anchors and calendar management. Vanilla JS.
  No CDN.

**Tests.**

- Unit tests for offset arithmetic across DST boundaries.
- Unit tests for paired-anchor state correctness across local midnight.
- Threshold-anchor unit test using a 30-day moon-illumination series.
- On the lab camera: create an anchor "30 min before sunrise"; verify
  it appears in Action Rules; bind to MQTT; verify firing time.
- Restart-idempotency: rebooting the camera produces the same set of
  registered topics with no flapping in operator-bound action rules.

**Exit.** Tag `v0.6.0`.

---

## M7 — Polished UI, logging, status panel (release v0.7.0)

**Goal.** Production-quality operator experience: clear status,
field-debuggable logs, configuration export/import.

**Deliverables.**

- Status panel ([FR-11.2](./requirements/11-configuration-ui.md)): last
  recompute time + reason, next recompute, current lat/lon and TZ in
  use, recent recompute summaries (in-memory ring buffer, last 50).
- Debug-logging toggle persisted via AXParameter
  ([FR-13.4](./requirements/13-logging.md)).
- Configuration export (JSON download) and import (with schema
  validation) per
  [FR-12.3](./requirements/12-configuration-persistence.md).
- All form fields validated server-side
  ([FR-11.5](./requirements/11-configuration-ui.md)).
- All controls keyboard-reachable
  ([FR-11.4](./requirements/11-configuration-ui.md)).

**Tests.**

- 24-hour soak run on the OS 12 lab camera with all events configured;
  verify ring buffer entries, no memory leak (RSS stable), no AppArmor
  violations.
- Export → wipe `localdata/` → import → recompute → identical event
  set.
- UI loads with the camera's outbound network blocked at the firewall;
  no broken assets.

**Exit.** Tag `v0.7.0`.

---

## M8 — Beta release readiness (release v1.0.0-beta)

**Goal.** First public 1.x release: feature-complete on armv7hf,
public CI, license audit, reproducible builds, polished docs. Signing
and Artpec-8+ hardware verification are deferred to a post-beta
milestone per [DL-23](./requirements/28-decision-log.md).

**Deliverables.**

- License-audit step in CI that fails on unapproved bundled licenses
  ([NFR-6](./requirements/20-non-functional.md)).
- Reproducible build verification: clean checkout → identical SHA-256
  on `.eap` artifacts. (No signature to mod-out, since signing is
  deferred per DL-23.)
- Documentation: `CONTRIBUTING.md` filled in (DCO, build steps, PR
  workflow); `SECURITY.md` describes the private disclosure channel;
  `CHANGELOG.md` covers v0.1 → v1.0.0-beta.
- Release page on GitHub: **unsigned** `.eap` × 2 (armv7hf + aarch64),
  SHA-256 checksums, `THIRD_PARTY_LICENSES.md`, `CHANGELOG.md`. The
  release page notes the beta status and the deferred-signing posture.

**Tests.**

- Full acceptance suite from
  [`23-verification.md`](./requirements/23-verification.md) passes
  end-to-end on the two armv7hf lab cameras (OS 12.10.61 and OS
  11.11.192). aarch64 `.eap` builds in CI; hardware smoke is
  deferred to post-beta per DL-23.
- License-audit CI step rejects a PR that introduces an LGPL or
  GPL-licensed dependency.

**Exit.** Tag `v1.0.0-beta`. Announce.

---

## What's deliberately deferred past v1.0.0-beta

These are noted in the requirements but not in the v1.0.0-beta
critical path:

- **Axis Application Signing in CI** — formerly DL-11/BR-7/NFR-5;
  reopened post-beta when the signing key is in hand or when ACAP v12
  enforces signing in shipping firmware. Released-promoted
  `v1.0.0` (no `-beta` suffix) is gated on this work landing.
  ([DL-23](./requirements/28-decision-log.md))
- **Artpec-8+ (aarch64) lab smoke** — the build matrix continues to
  produce the aarch64 `.eap` and CI verifies it builds cleanly, but
  hardware smoke-test on real Artpec-8+ silicon waits for hardware
  acquisition. Promotion to `v1.0.0` GA is gated on this.
  ([DL-23](./requirements/28-decision-log.md))
- Map-based location picker (re-evaluate after v1; would require
  bundling Leaflet + a tile fallback strategy).
- Path B — programmatically inject VAPIX action rules
  ([DL-05](./requirements/28-decision-log.md)) — quality-of-life add-on,
  reconsider only if operators ask.
- GPS-mobile camera support
  ([DL-12](./requirements/28-decision-log.md)) — defer until a real
  user case appears.
- Holiday database — out of scope by design
  ([FR-6](./requirements/06-user-calendar-dates.md)).

---

## Sequencing rationale

- **M1 before any feature work** because the build-and-install loop
  catches the most expensive-to-debug class of bug (manifest, signing,
  sandboxing, framework wiring) at zero feature complexity.
- **M2 is sunrise/sunset only** rather than the full FR-3 set so that
  the timer wiring is exercised end-to-end before lunar/seasonal/anchor
  modules pile complexity on top.
- **M4 lunar before M6 anchors** because numeric-threshold anchors
  ([FR-7.7](./requirements/07-schedule-anchors.md)) consume moon
  illumination — they need lunar.c in place.
- **M7 polish before M8 release** rather than mixed because polish
  iteration cycles tend to surface new issues; doing them all under
  one milestone bounds scope.
- **M8 leaves Artpec-8+ acquisition until the end** because every
  earlier milestone runs fine on the existing armv7hf lab cameras; the
  aarch64 hardware is only needed for v1 release validation.
