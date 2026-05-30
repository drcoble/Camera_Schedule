# Changelog

All notable changes to Camera_Schedule are documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning 2.0](https://semver.org/spec/v2.0.0.html)
per [DR-13](./requirements/25-licensing-and-distribution.md). Format
choice rationale: see [DL-24](./requirements/28-decision-log.md).

Tag dates reflect the date the annotated tag was cut. The git tag is
the canonical artifact reference.

## [Unreleased]

(no changes since v1.0.1)

## [1.0.1] — 2026-05-30

Maintenance release: configuration-UI fixes, an opt-in firing
default, and front-end cache-busting. Artifacts remain **unsigned**
and Artpec-8+ / aarch64 hardware smoke is still deferred per
[DL-23](./requirements/28-decision-log.md). armv7hf lab-verified on
AXIS OS 11.11+ and OS 12.x.

### Added

- The **About** page now shows a link to the source repository, read
  from the manifest `vendorUrl` so it cannot drift.

### Changed

- **Built-in events now default to OFF on a clean install.** A fresh
  install seeds every built-in solar/lunar/seasonal topic disabled; the
  operator opts in per event from the Schedule UI. Topics are still
  declared (Action Rules bindings are unaffected). Operator-defined
  schedules and upgrade-added built-ins keep the absent-key = enabled
  default ([DL-29](./requirements/28-decision-log.md),
  [FR-11.7](./requirements/11-configuration-ui.md)).
- Static front-end assets (`js/*.js`, `css/*.css`) are cache-busted via
  a content-hash `?v=` query injected at build time, so browsers no
  longer serve stale UI code across app updates
  ([DL-28](./requirements/28-decision-log.md)).
- App version and source-repo URL are read from the manifest at
  runtime (single source of truth), replacing a hardcoded version
  constant that had drifted to `0.7.0`.
- README rewritten around application architecture and functionality.

### Fixed

- **Anchors and Calendar pages were unusable** — stuck on "Loading…"
  with a non-functional "New entry" button — due to a JavaScript
  syntax error (unescaped quotes) in the delete-confirm string that
  aborted the whole page script.
- The **Save** button stayed disabled after a successful edit, so a
  second edit could not be saved without reloading the page.

## [1.0.0-beta] — 2026-05-07

First public 1.x release. Feature-complete on armv7hf and
lab-verified on AXIS OS 11.11+ and OS 12.x. Artpec-8+ / aarch64
hardware smoke and Axis Application Signing are deferred to a
post-beta milestone per
[DL-23](./requirements/28-decision-log.md). Acceptance evidence:
[`docs/verification/v1.0.0-beta-readiness.md`](./docs/verification/v1.0.0-beta-readiness.md)
(9 PASS, 1 PARTIAL, 0 FAIL).

### Added

- License-audit CI gate (`.github/workflows/license-audit.yml`)
  that fails the build on bundled dependencies whose licenses are
  not on the approved list. Hand-rolled stdlib-only audit script
  at `app/scripts/license_audit.py` (DL-25 rationale)
  ([NFR-6](./requirements/20-non-functional.md),
  [DR-11](./requirements/25-licensing-and-distribution.md)).
- Reproducible-build verification
  (`.github/workflows/reproducibility.yml`): a clean checkout
  produces an `.eap` whose SHA-256 matches a second build of the
  same source tree. `SOURCE_DATE_EPOCH` is derived from the head
  commit; `app/scripts/repack_eap.sh` normalizes tar mtimes,
  ordering, ownership, and gzip headers (DL-26)
  ([BR-6](./requirements/22-build-and-packaging.md)).
- Release-publish workflow (`.github/workflows/release.yml`) that
  on a `v[0-9]+.[0-9]+.[0-9]+(-beta)?` tag push: builds both `.eap`
  with the SDK image digest pinned, generates `SHA-256SUMS.txt`,
  drafts a GitHub Release with assets attached. Always-draft
  policy lets the integrator review the bundle before publishing
  (DL-27)
  ([DR-12](./requirements/25-licensing-and-distribution.md)).
- `THIRD_PARTY_LICENSES.md` enumerating every bundled third-party
  component, its version, and its license text
  ([DR-10](./requirements/25-licensing-and-distribution.md)).
- `approved-licenses.txt` allowlist at the repo root.
- `app/scripts/extract_release_notes.py` extracts the matching
  `[1.0.0-beta]` (or any tagged) section from `CHANGELOG.md` for
  the GitHub Release body.
- `CHANGELOG.md` itself, covering the v0.1 → v1.0.0-beta history
  in [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/)
  format (DL-24).
- `docs/verification/v1.0.0-beta-readiness.md` — frozen acceptance
  report against [`requirements/23-verification.md`](./requirements/23-verification.md).
- `docs/release-pipeline.md` — single contributor reference for
  the M8 CI/release machinery.
- DCO sign-off policy in `CONTRIBUTING.md` (DL-24 tightens
  [DR-7](./requirements/25-licensing-and-distribution.md) from
  "recommended" to "mandatory").
- Public release page on GitHub with **unsigned** `.eap` artifacts
  for both armv7hf and aarch64, SHA-256 checksums, and the
  third-party-license bundle.

### Changed

- `SECURITY.md` rewritten to document the supported-versions
  table, the response-window commitment, and the in-scope /
  out-of-scope policy. Reporting channel narrowed to **GitHub
  private vulnerability reporting only**; email fallback removed.
  Signing-pipeline language amended to reflect
  [DL-23](./requirements/28-decision-log.md).
- README scope-at-a-glance corrected from the dropped Path B
  language ("Generate RFC 5545 iCalendar... Event Schedule API")
  to the actual Path A ACAP event-topic mechanism
  ([DL-05](./requirements/28-decision-log.md)). Banner notes
  `v1.0.0-beta` status and the gates on promotion to `v1.0.0`.

### Verified

- 24-hour soak on the OS 12 lab camera (10.1.40.113): RSS stable
  at 9032 kB across 454 samples spanning 25h32m wall-clock; 0
  AppArmor DENIED entries, 99.78% HTTP 200, 0 recompute errors.
  Methodology caveat (host-sleep gaps) documented in the
  verification report.
- License-audit gate exercised end-to-end via a synthetic LGPL
  drop PR (#2). The audit job rejected the LGPL-2.1-or-later
  header in 6 seconds (CI run `25524553292`); PR closed without
  merging, branch deleted.
- All 22 built-in AXEvent topics (10 solar + 8 lunar + 4 seasonal)
  declared and operationally verified across M2..M5 lab gates and
  the M6 anchor-CRUD lab gate. Tag candidate is built from the
  same tree as v0.7.0 plus M8's CI/build/docs additions; no
  source-code changes between v0.7.0 and v1.0.0-beta.

## [0.7.0] — 2026-05-06

M7 — operator-grade UI, status panel, configuration export/import,
and runtime debug toggle.

### Added

- Status panel with last/next recompute, current lat/lon and TZ,
  and a 50-entry in-memory recompute ring buffer
  ([FR-11.2](./requirements/11-configuration-ui.md),
  [FR-13.3](./requirements/13-logging.md)).
- "Recompute now" button
  ([FR-10.2](./requirements/10-recompute-cadence.md)).
- Configuration export/import endpoints with the
  `camera-schedule.config.v1` envelope, schema validation, and
  atomic rollback on import failure
  ([FR-12.3](./requirements/12-configuration-persistence.md)).
- Four scalar settings exposed as AXParameters under
  `root.Camera_schedule.*`: `LookaheadDays`, `EventNamePrefix`,
  `PollIntervalSeconds`, `DebugLogging`
  ([FR-12.2](./requirements/12-configuration-persistence.md)).
- Persistent debug-logging toggle drives a runtime `LOG_DBG`
  gate; new shared `app/src/log.h` lifts logging macros out of
  `main.c` so all translation units share one header
  ([FR-13.4](./requirements/13-logging.md)).
- Five new FastCGI endpoints: `state`, `recompute`, `export`,
  `import`, `debug`.
- `rss_kb` field on `GET /state`, populated from
  `/proc/self/status:VmRSS`, for the soak harness
  ([DL-21](./requirements/28-decision-log.md)).
- Formal accessibility audit across all five HTML pages
  ([FR-11.4](./requirements/11-configuration-ui.md)).
- Host fixture `test_export_import.c`: round-trip identity plus
  10 reject cases (12/12 pass).
- 24-hour soak harness `app/test/lab/soak_24h.sh`.
- Recompute coalescing for back-to-back triggers
  ([FR-10.3](./requirements/10-recompute-cadence.md),
  [FR-13.3](./requirements/13-logging.md)).

### Changed

- M7 status endpoint renamed `/status` → `/state` to avoid a
  silent collision with the vendored Timelapse2 framework's
  `/status` node registered at `ACAP.c:691`
  ([DL-22](./requirements/28-decision-log.md)).

## [0.6.0] — 2026-05-06

M4, M5, and M6 shipped together. M6's acceptance gate (operator
UI + CRUD round-trip) cleared; M4's full-moon and M5's June-solstice
firing gates verify retroactively on 2026-05-31 and 2026-06-21.

### Added

- **Lunar events (M4)**: `moonrise`, `moonset`, `moonnoon`,
  `moonmidnight`, `newmoon`, `firstquarter`, `fullmoon`,
  `lastquarter`. Parallax-aware rise/set via Meeus *Astronomical
  Algorithms* ch. 47 hourly altitude sampling; phase events via
  ch. 49 ([FR-4](./requirements/04-lunar-events.md)).
- **Seasonal events (M5)**: `marchequinox`, `junesolstice`,
  `septemberequinox`, `decembersolstice`. Meeus ch. 27 closed-form
  with Table 27.C corrections and Espenak-Meeus ΔT
  ([FR-5](./requirements/05-seasonal-events.md)). Hemisphere-aware
  `Longest Day` / `Shortest Day` labels rebound at boot and on
  every location update.
- **Anchors (M6)**: operator-defined schedule entries with offset,
  paired-interval, and numeric-threshold variants
  ([FR-7](./requirements/07-schedule-anchors.md)). Persisted in
  `localdata/anchors.json`.
- **User calendar (M6)**: single-date, date-range, and
  annual-recurring entries
  ([FR-6](./requirements/06-user-calendar-dates.md)). Persisted in
  `localdata/calendar.json`.
- **Unified Schedule list view (M6)**: built-in + user-defined
  schedules in one collapsible category UI with per-row
  enable/disable toggle
  ([FR-11.6](./requirements/11-configuration-ui.md)).
- **Firing-path enable gate (M6)**: disabled topics stay declared
  in the AXEvent table; only the timer arming step skips them
  ([FR-11.7](./requirements/11-configuration-ui.md),
  [DL-18](./requirements/28-decision-log.md)). Persisted in
  `localdata/schedule_enabled.json`.
- Atomic-write helper in `app/src/persistence.c`: write-temp +
  fsync + parse-back + schema-validate + rename
  ([FR-12.1](./requirements/12-configuration-persistence.md)).
- Four new FastCGI endpoints: `anchors`, `calendar`, `events`,
  `events_today`.
- Host fixtures: `test_lunar.c`, `test_seasonal.c`,
  `test_anchors.c`, `test_calendar.c`.
- Seasonal-event accuracy budget scoped to 1900–2050; 2050+ is
  best-effort due to ΔT prediction divergence
  ([DL-19](./requirements/28-decision-log.md)).

### Fixed

- `ACAP_EVENTS_Add_Event` return-value check was inverted (the
  function returns the declarationID; non-zero means success, not
  failure).
- Implemented `ACAP_FILE_Exists` in vendored `ACAP.c` (the
  upstream framework declared but did not define it).
- `ACAP_HTTP_Respond_JSON` truncated payloads larger than 4 KB
  to nothing.
- M6 build errors: `LOG_ERR` collision with syslog header, missing
  forward declaration, ordering issue in `main.c`.
- DST epoch constants in `test_anchors.c` corrected to 2026 dates.

## [0.3.0] — 2026-05-06

M3 — full solar suite and polar-latitude safety.

### Added

- Civil, nautical, and astronomical twilight pairs (zeniths 96°,
  102°, 108°), plus solar noon and solar midnight
  ([FR-3](./requirements/03-solar-events.md)).
- Polar-latitude safety: `solar.c` returns a `NO_EVENT_TODAY`
  sentinel when `acos` would overflow, and the timer arming step
  skips that day with an INFO-level log instead of crashing or
  arming a bogus timer.
- Host fixtures expanded to cover latitudes 70°N and 78°N across
  the winter solstice and DST spring-forward / fall-back
  boundaries.

## [0.2.0] — 2026-05-06

M2 — sunrise / sunset MVP. The first release where an operator
can install the app, configure lat/lon, and bind a camera action
rule to a working sunrise or sunset trigger.

### Added

- `app/src/astro/solar.c` solar-position routine, refactored from
  Timelapse2's `sunevents.c` and parameterized by zenith angle.
- `sunrise` and `sunset` AXEvent topics published via
  `app/settings/events.json`.
- `app/src/timers.c` midnight scheduler with per-event one-shot
  timers via `g_timeout_source_new_seconds`
  ([FR-9](./requirements/09-event-firing.md)).
- Location UI: `app/html/location.html` plus the `location`
  FastCGI endpoint, which writes back through
  `/axis-cgi/geolocation/set.cgi`
  ([DL-07](./requirements/28-decision-log.md)).
- Host fixture `test_solar.c` comparing computed sunrise/sunset
  against USNO data.
- Lat/lon UI inputs use `step="any"` and round read-back to
  microdegree precision ([FR-1.6](./requirements/01-geo-location.md),
  [DL-17](./requirements/28-decision-log.md)).
- Schedule enable/disable design codified
  ([DL-18](./requirements/28-decision-log.md)).

### Fixed

- `settings/events.json` was being silently dropped from the
  produced `.eap` because `acap-build` only auto-bundles
  `html/`, `lib/`, and a fixed set of standard files. Added an
  explicit `-a settings/events.json` to the `acap-build`
  invocation in `app/Dockerfile`.

## [0.1.0] — 2026-05-05

M1 — build pipeline and empty `.eap`. The first tagged artifact;
proves the manifest, signing-stub, sandboxing, and framework
vendoring work end-to-end before any feature work begins.

### Added

- `app/manifest.json` with schemaVersion 1.7.x,
  `embeddedSdkVersion: "3.0"`, `runMode: "respawn"`, and a single
  `httpConfig` entry for the `about` FastCGI endpoint.
- `app/Dockerfile` pinned to
  `axisecp/acap-native-sdk:12.6.0-${ARCH}-ubuntu24.04`,
  parameterized by `--build-arg ARCH=`.
- `app/Makefile` with `build-armv7hf`, `build-aarch64`,
  `build-all`, and `clean` targets.
- Vendored `ACAP.c`, `ACAP.h`, `cJSON.c`, `cJSON.h` from
  Timelapse2 (MIT, copyright Fred Juhlin) into `app/src/acap/`,
  attribution recorded in `THIRD_PARTY_LICENSES.md`
  ([DL-06](./requirements/28-decision-log.md)).
- `app/src/main.c` boots a GLib main loop and registers the
  `about` endpoint.
- Minimal `app/html/about.html` + `app/html/js/app.js` that GETs
  the `about` endpoint and renders version + arch.
- CI workflow that builds both `.eap` artifacts and uploads them.
- `LICENSE` (MIT), `.gitignore`, `CONTRIBUTING.md` (placeholder),
  `SECURITY.md` (placeholder), `CODE_OF_CONDUCT.md`,
  `THIRD_PARTY_LICENSES.md`.

### Removed

- `compatibleOsVersions` from the manifest. The field does not
  exist in any manifest schema bundled with SDK 12.6.0
  (verified v1.0…v1.8.0). OS compatibility is enforced at runtime
  by the APIs the app links against
  ([DL-16](./requirements/28-decision-log.md), supersedes
  [DL-04](./requirements/28-decision-log.md)).

[Unreleased]: https://github.com/drcoble/Camera_Schedule/compare/v1.0.0-beta...HEAD
[1.0.0-beta]: https://github.com/drcoble/Camera_Schedule/releases/tag/v1.0.0-beta
[0.7.0]: https://github.com/drcoble/Camera_Schedule/releases/tag/v0.7.0
[0.6.0]: https://github.com/drcoble/Camera_Schedule/releases/tag/v0.6.0
[0.3.0]: https://github.com/drcoble/Camera_Schedule/releases/tag/v0.3.0
[0.2.0]: https://github.com/drcoble/Camera_Schedule/releases/tag/v0.2.0
[0.1.0]: https://github.com/drcoble/Camera_Schedule/releases/tag/v0.1.0
