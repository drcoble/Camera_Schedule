# Decision log

A chronological record of substantive design decisions for Camera_Schedule,
including requirements that were **removed** or **replaced** and the
rationale at the time. New entries are appended at the bottom; entries
SHALL NOT be edited after they're recorded except to add a "superseded by
DL-NN" pointer when a later decision reverses an earlier one.

Each entry uses the schema:

```
## DL-NN — <short title>
Date: YYYY-MM-DD  |  Status: <accepted|superseded>
Decision: <one paragraph>
Rationale: <one paragraph>
Removed / changed: <bullet list>
References: <links>
```

---

## DL-01 — Project license is MIT

Date: 2026-05-04  |  Status: accepted

**Decision.** The project ships under the MIT License, declared in
`/LICENSE` with SPDX identifier `MIT` in every project-owned source file.

**Rationale.** Earlier draft recommended Apache-2.0 for its patent grant.
The user requested "the simpler license that is the least restrictive."
MIT is the canonical short-form permissive license: minimal restrictions,
universally recognized, trivially compatible with downstream LGPL/GPL
consumers, and removes the Apache-only `NOTICE` file requirement.

**Removed / changed.**
- Apache-2.0 recommendation (DR-1) replaced with MIT.
- `NOTICE` file requirement (former DR-3) removed; bundled-dependency
  attributions now live in `THIRD_PARTY_LICENSES.md` only.
- OQ-9 closed.

**References.**
[25-licensing-and-distribution.md](./25-licensing-and-distribution.md)
DR-1, DR-3.

---

## DL-02 — Open source on a public repository

Date: 2026-05-04  |  Status: accepted

**Decision.** Source, issue tracker, CI, and tagged release artifacts
are public. Build SHALL be reproducible from public sources; signing key
is the only private piece.

**Rationale.** Project is community-oriented; broad reuse and external
contribution are explicit goals. Public CI (BR-5) gives third parties
verifiable build output.

**Removed / changed.**
- NFR-6 license posture relaxed: the previous blanket LGPL prohibition
  (driven by commercial-redistribution worries) is gone. LGPL is now
  permitted with an `lgpl-relink/` artifact at release time. NREL SPA
  remains banned (incompatible with any OSI license).

**References.** [20-non-functional.md](./20-non-functional.md) NFR-6,
[22-build-and-packaging.md](./22-build-and-packaging.md) BR-5/BR-8,
[25-licensing-and-distribution.md](./25-licensing-and-distribution.md).

---

## DL-03 — Architecture matrix corrected: armv7hf supported on AXIS OS 12

Date: 2026-05-05  |  Status: accepted

**Decision.** Build pipeline produces three `.eap` artifacts:
`os12-aarch64.eap`, `os12-armv7hf.eap`, `os11-armv7hf.eap`. Both lab
cameras (one OS 11.10.83, one OS 12.10.61) are Artpec-7 / armv7hf, which
disproved the "v12 = aarch64 only" assumption from public docs.

**Rationale.** Hardware probing showed Artpec-7 cameras run AXIS OS 12
on armv7hf. Excluding armv7hf from the OS 12 build matrix would have
locked out a substantial install base.

**Removed / changed.**
- PR-1 "Architecture aarch64 (only)" → "aarch64 AND armv7hf".
- PR-2 single-arch armv7hf clarified.
- PR-3 multi-build matrix expanded from 2 to 3 artifacts.

**References.** [21-platform-compatibility.md](./21-platform-compatibility.md),
[26-discovered-environment.md](./26-discovered-environment.md).

---

## DL-04 — Manifest declares OS compatibility via `compatibleOsVersions`

Date: 2026-05-05  |  Status: accepted

**Decision.** Manifest SHALL use the `compatibleOsVersions` field with
`versionRange[].{min, max}` entries to declare supported AXIS OS
versions. v12 build: `Min=12 Max=13` (or current ceiling). v11 build (if
schema supports it): `Min=11 Max=11`.

**Rationale.** Probing the camera's installed-app list surfaced this
field — used by AXIS Object Analytics (`Min=12 Max=12`) and VMD
(`Min=12.10 Max=13`). Earlier doc claim "no documented field" was wrong.

**Removed / changed.**
- OQ-1 closed positive.

**References.** [21-platform-compatibility.md](./21-platform-compatibility.md)
PR-1/PR-2.

---

## DL-05 — Schedule injection via ACAP event topics (Path A), not iCalendar

Date: 2026-05-05  |  Status: accepted

**Decision.** The app publishes its computed times as **ACAP event
topics** registered with the device event engine; operators bind their
existing camera action rules to those topics. The originally specified
"generate iCalendar payloads and push them to the Event Schedule REST
API" approach is dropped.

**Rationale.** Hardware probing showed
`/config/rest/scheduled-events/v{1,2}` does not exist on either lab
camera (OS 11.10.83 nor OS 12.10.61), and SOAP `GetSchedules` /
`GetRecurrenceRules` return "not implemented." Path A — the model
Timelapse2 (MIT) already uses on the lab OS 12 camera with its `sunnoon`
topic — is the only working surface and is also strictly simpler: no
reconciliation loop, no per-day VEVENT expansion, no UID stability
problem.

**Removed / changed.**
- **FR-8 (iCalendar generation)** — entirely removed. RFC 5545
  conformance, pulse vs interval VEVENT handling, per-day expansion
  windows, stable app prefix on schedule names, deterministic UIDs. None
  apply under Path A. The file is repurposed as "Event registration."
- **FR-9 (VAPIX schedule writes)** — entirely removed. POST/PATCH/DELETE
  to `/config/rest/scheduled-events/v2/schedules`, D-Bus credential
  acquisition specifically for that endpoint, the prefix-filtered
  reconciliation loop, the 30 s → 30 min retry budget against that API,
  the schedule-name prefix mechanism. The file is repurposed as
  "Event firing."
- **FR-7 (schedule anchors)** — anchor data model preserved; expansion
  semantics changed from "produce iCalendar VEVENTs" to "register a
  distinct ACAP event topic per anchor and arm a GLib timer to fire it
  at the computed local time."
- **OQ-5** (Event Schedule API quotas) — moot.
- **OQ-6** closed (negatively).
- **OQ-10** closed (Path A confirmed).

**References.** [26-discovered-environment.md](./26-discovered-environment.md),
[27-reuse-from-timelapse2.md](./27-reuse-from-timelapse2.md),
[07-schedule-anchors.md](./07-schedule-anchors.md),
[08-event-registration.md](./08-event-registration.md),
[09-event-firing.md](./09-event-firing.md).

---

## DL-06 — Reuse Timelapse2's ACAP mini-framework

Date: 2026-05-05  |  Status: accepted

**Decision.** Vendor `ACAP.c`, `ACAP.h`, `cJSON.c`, `cJSON.h` from
Fred Juhlin's Timelapse2 repo (MIT) into `app/src/acap/` with original
copyright lines preserved. Adopt its declarative-events pattern
(`settings/events.json` → `ACAP_EVENTS_Add_Event` at boot) and its
midnight + per-event GLib timer model.

**Rationale.** Timelapse2's MIT license matches ours, and the framework
covers ~60% of Path A's plumbing (HTTP/FastCGI, AXEvent declare/fire,
VAPIX loopback with D-Bus credentials, AXParameter-style config,
`ACAP_DEVICE_*` helpers). Building this from scratch would duplicate
~80 KB of well-tested code for no benefit.

**Removed / changed.**
- BR-2 references the Timelapse2 solar core as the starting point for
  our solar module (refactored to be zenith-parameterized for FR-3
  twilights).
- New BR-2a covers framework reuse rules and attribution.

**References.** [27-reuse-from-timelapse2.md](./27-reuse-from-timelapse2.md),
[22-build-and-packaging.md](./22-build-and-packaging.md) BR-2/BR-2a.

---

## DL-07 — Lat/lon override writes back to camera geolocation service

Date: 2026-05-05  |  Status: accepted

**Decision.** When the operator enters a manual lat/lon override in the
app's UI, the app writes it back to the camera's geolocation service
via `/axis-cgi/geolocation/set.cgi`. The app does **not** maintain a
separate persistent override in `localdata/` or AXParameter.

**Rationale.** Matches Timelapse2's pattern. Single source of truth,
survives reflash, visible to other apps. Side effect: if multiple apps
on the same camera use geolocation, they share one value — acceptable
because the "what is this camera's location" question has only one
correct answer.

**Removed / changed.**
- FR-1.3 reworded: override is no longer "stored in app config."
- FR-12.2 amended: lat/lon dropped from the AXParameter-exposed scalar
  set.

**References.** [01-geo-location.md](./01-geo-location.md) FR-1.3,
[12-configuration-persistence.md](./12-configuration-persistence.md)
FR-12.2.

---

## DL-08 — No external CDN for UI assets (departure from Timelapse2)

Date: 2026-05-05  |  Status: accepted

**Decision.** All UI assets — Bootstrap, jQuery (if used), any map
library — SHALL be served from the app's own `/local/<appName>/` path.
For v1 the location UI is a numeric lat/lon form plus a "use camera
GPS-set location" button; no embedded map.

**Rationale.** Timelapse2 pulls Leaflet from `unpkg.com` and OSM tiles
from public servers, which breaks on air-gapped camera networks. We
deliberately diverge to keep the app fully functional offline.

**Removed / changed.**
- FR-11.3 expanded to explicitly forbid map-tile and CDN loads and to
  define the v1 location-UI fallback.
- Implicit "Leaflet map" expectation (carried over from looking at
  Timelapse2) replaced with a numeric form.

**References.** [11-configuration-ui.md](./11-configuration-ui.md)
FR-11.3, [27-reuse-from-timelapse2.md](./27-reuse-from-timelapse2.md).

---

## DL-09 — Test-camera credentials never enter the repository

Date: 2026-05-05  |  Status: accepted

**Decision.** Lab camera IPs and root credentials are stored in the
developer's local Claude memory and any scripts SHALL source them from
environment variables, never from in-tree files.

**Rationale.** Repo is public-MIT. Even though these are lab cameras on
an internal network, embedding credentials in a public repo is a hard
"no" and creates a foot-gun for contributors who fork.

**References.** [23-verification.md](./23-verification.md) (test
environment subsection), [24-open-questions.md](./24-open-questions.md)
preamble.

---

## DL-10 — Rely on AXIS OS zoneinfo; do not vendor tzdata

Date: 2026-05-05  |  Status: accepted

**Decision.** The app SHALL use the camera's system zoneinfo via
standard C `localtime_r()` / `tm_gmtoff`. No tzdata snapshot SHALL be
vendored into the .eap.

**Rationale.** Direct evidence from the OS 12.10.61 lab camera: the
installed Timelapse2 ACAP — which runs in the same v12 dynamic-user
sandbox we'll target — returned sunrise `1777977777` (2026-05-05
06:42 EDT) and sunset `1778026909` (2026-05-05 20:21 EDT) for the
configured Atlanta location. Those are correct local times with DST
applied, which means the sandbox successfully reads
`/usr/share/zoneinfo/America/New_York` and `localtime_r()` returns the
right offset. Vendoring tzdata would add ~200 KB to the .eap to
duplicate functionality the OS already provides.

**Removed / changed.**
- OQ-2 closed positive.
- FR-2 reliance on system zoneinfo is no longer "provisional"; the
  fallback "vendor tzdata if missing" plan is dropped.

**References.** [02-time-and-timezone.md](./02-time-and-timezone.md),
[26-discovered-environment.md](./26-discovered-environment.md).

---

## DL-11 — Signing workflow: GitHub Actions on tagged main

Date: 2026-05-05  |  Status: accepted

**Decision.** Application signing follows
[BR-7](./22-build-and-packaging.md):

- Signing key is held as a **GitHub Actions secret** in the public
  repository's organization settings.
- Signing runs **only** on workflow dispatches triggered by a tag push
  on the protected `main` branch (no PR builds, no fork builds).
- Unsigned dev builds are produced by the same Makefile targets but
  with the signing step skipped — anyone can build them from public
  source.
- Key rotation: documented in `SECURITY.md`; planned cadence yearly or
  on suspected compromise.
- Obtaining the signing key from Axis is an implementation prerequisite,
  not a requirement to be re-decided.

**Rationale.** Standard pattern for a single-maintainer open-source
ACAP project. Keeps the key off contributors' machines, off CI for
untrusted PRs, and off the source tree.

**Removed / changed.**
- OQ-3 closed.

**References.** [22-build-and-packaging.md](./22-build-and-packaging.md)
BR-7, [25-licensing-and-distribution.md](./25-licensing-and-distribution.md)
DR-9.

---

## DL-12 — GPS-mobile cameras out of scope for v1

Date: 2026-05-05  |  Status: accepted

**Decision.** Cameras with on-board GPS that report dynamically
changing lat/lon (typically PTZ-mobile installations) are explicitly
**out of scope for v1**. The 1-hour Geolocation API poll
([FR-1.1](./01-geo-location.md)) stays as the default. A future "track
GPS" mode with shorter polling MAY be revisited if customer demand
emerges.

**Rationale.** Neither lab camera has GPS. Treating GPS-mobile cases
as a v1 feature would expand testing scope and introduce edge cases
(continuous recompute, schedule churn during slow movement) without a
concrete user. Easier to ship the fixed-mount product first.

**Removed / changed.**
- OQ-4 closed.

**References.** [01-geo-location.md](./01-geo-location.md), README "Out
of scope" section (already lists "GPS hardware integration").

---

## DL-13 — Reverse-proxy auth: rely on manifest httpConfig.access

Date: 2026-05-05  |  Status: accepted

**Decision.** Web-UI role gating ([FR-11.1](./11-configuration-ui.md))
SHALL use the manifest `httpConfig` `access` field per FastCGI endpoint
(`admin` for write endpoints, `viewer` for read-only). No additional
D-Bus method declaration is required for role propagation under the v12
dynamic-user model.

**Rationale.** Timelapse2 (in the v12 sandbox on the lab camera) uses
exactly this pattern and works — its sunevents endpoint requires admin
auth and rejected our unauthenticated probes with HTTP 401 before
accepting digest credentials. Direct evidence the path is supported.

**Removed / changed.**
- OQ-7 closed positive.

**References.** [11-configuration-ui.md](./11-configuration-ui.md),
[27-reuse-from-timelapse2.md](./27-reuse-from-timelapse2.md).

---

## DL-14 — Anchor cap fixed at 64 for v1

Date: 2026-05-05  |  Status: accepted

**Decision.** The soft cap of 64 operator-defined anchors
([FR-7.3](./07-schedule-anchors.md)) is locked for v1.

**Rationale.** No technical limit drives the number; 64 is a UI
rendering judgment that comfortably accommodates "every solar event ×
common offsets × user calendar entries × room to grow." Real
deployment shapes will tell us if it needs raising.

**Removed / changed.**
- OQ-8 closed.

**References.** [07-schedule-anchors.md](./07-schedule-anchors.md)
FR-7.3.

---

## DL-15 — OS floor is AXIS OS 11.11; build matrix simplifies to 2 artifacts

Date: 2026-05-05  |  Status: accepted

**Decision.** Camera_Schedule supports **AXIS OS 11.11.x and newer**.
Cameras running OS ≤ 11.10 are unsupported. Build matrix collapses
from 3 .eap artifacts to **2**:

| Artifact | SDK | Manifest | Architecture | OS targets |
|---|---|---|---|---|
| `os11-12-armv7hf.eap` | acap-native-sdk **12.x** | schemaVersion 1.7.x | armv7hf | OS 11.11+ and OS 12.x on Artpec-7 |
| `os12-aarch64.eap` | acap-native-sdk **12.x** | schemaVersion 1.7.x | aarch64 | OS 12.x on Artpec-8+ |

Both are signed and built from a single source tree with one manifest
schema. (OS 11 = armv7hf only; aarch64 only exists for OS 12-eligible
SoCs, so there is no OS-11 aarch64 artifact.)

**Rationale.** Three drivers:

1. **Hardware reality.** The lab OS 11 unit (11.10.83) is *below* this
   floor and Timelapse2 itself excludes ≤11.10. Anyone wanting to run
   Camera_Schedule on OS 11 needs at least 11.11, which has been
   shipping for over a year.
2. **Schema unification.** Manifest 1.7.x is accepted by both OS 11.11+
   and OS 12.x, eliminating the need for two manifest files and two
   SDK versions.
3. **Reuse alignment.** Timelapse2 (the framework we vendor) uses
   exactly this pattern — single SDK 12.x build, schema 1.7.1, runs on
   both OS 11.11+ and OS 12.

**Removed / changed.**
- PR-1 / PR-2 reduced to a single set of platform requirements — see
  the rewritten [21-platform-compatibility.md](./21-platform-compatibility.md).
- PR-3 build matrix dropped from 3 artifacts to 2.
- The separate `manifest.v11.json` and `Dockerfile.os11-armv7hf` planned
  in [22-build-and-packaging.md](./22-build-and-packaging.md) are
  removed; the v11-friendly armv7hf build now uses the same manifest
  and Dockerfile as the OS 12 armv7hf build.
- The "v11 may use legacy root path" allowance is dropped; the
  dynamic-user + D-Bus credential pattern is used uniformly.
- The lab OS 11.10.83 camera is **not** a supported target; for OS 11
  testing it must be upgraded to 11.11 or later.
- OQ-11 closed.

**References.** [21-platform-compatibility.md](./21-platform-compatibility.md),
[22-build-and-packaging.md](./22-build-and-packaging.md),
[27-reuse-from-timelapse2.md](./27-reuse-from-timelapse2.md).

---

## DL-16 — `compatibleOsVersions` is not a manifest schema field; remove from manifest

Date: 2026-05-05  |  Status: accepted

**Decision.** Drop the `compatibleOsVersions` declaration from
`app/manifest.json`. PR-1 is amended to remove the requirement that
the manifest declare it.

**Rationale.** The field doesn't exist. CI run `25398126081` introspected
every manifest schema bundled with `axisecp/acap-native-sdk:12.6.0`
(versions `application-manifest-schema-v1.0.json` through
`application-manifest-schema-v1.8.0.json`). The schemas have
`additionalProperties: False` at every level and no schema declares
a `compatibleOsVersions` property at any path. The discovery job's
diagnostic output:

```
top-level properties: ['$schema', 'acapPackageConf', 'resources', 'schemaVersion']
acapPackageConf properties: ['configuration', 'copyProtection',
                             'installation', 'setup', 'uninstallation']
locations of compatibleOsVersions: NOT FOUND
```

DL-04's claim that `compatibleOsVersions` was observed in installed
apps (Object Analytics with `Min=12 Max=12`, VMD with `Min=12.10
Max=13`) was either (a) misread — the field name on the camera might
have been different, or those values came from `embeddedSdkVersion` /
package metadata, not manifest schema; or (b) the field exists in a
manifest schema version newer than v1.8.0 not yet shipping in any
public SDK image we use.

**Practical impact.** OS compatibility is enforced at runtime: the app
links against APIs that only exist on OS 11.11+ (`embeddedSdkVersion:
3.0` plus the AXEvent calls in vendored ACAP.c), so install on an
older OS will fail at app launch even without a manifest declaration.
The user-facing degradation from removing the field is "the install
dialog doesn't preempt incompatible installs" — annoying but not a
functional gap.

**Removed / changed.**
- `app/manifest.json` no longer carries a `compatibleOsVersions` block.
- PR-1 amended (see [21-platform-compatibility.md](./21-platform-compatibility.md))
  to remove the manifest declaration requirement; the AXIS OS 11.11+
  floor is preserved as a release-notes / install-guidance commitment.
- DL-04 stays in the log (append-only) but its closing claim about
  this specific field is superseded here.
- OQ-12 closed.

**Forward look.** If a future SDK introduces the field, re-add it at
the schema-defined path and remove the schema-discovery CI job.

**References.** Discovery output in CI run 25398126081 (job
`SDK manifest-schema discovery`),
[21-platform-compatibility.md](./21-platform-compatibility.md),
[24-open-questions.md](./24-open-questions.md) OQ-12.

---

## DL-17 — UI rounds lat/lon read-back to microdegree precision

Date: 2026-05-05  |  Status: accepted

**Decision.** The configuration UI's lat/lon inputs use `step="any"` and
display read-back values rounded to 6 decimal places (microdegree
precision). The camera's stored value is unchanged unless the operator
explicitly saves a new value through the form. Internal computation
continues to use IEEE-754 `double` end-to-end; the rounding is purely
a UI-side display behavior. See FR-1.6.

**Rationale.** During the M2 lab install the location form rejected the
value the camera's geolocation service returned. The camera reports
lat/lon at higher precision than the form's previous `step="0.000001"`
constraint allowed, so the HTML5 step validator marked the field
invalid and `parseFloat`'d submissions failed. Microdegree precision
(~11 cm at the equator) is finer than any camera GPS hardware delivers
or any sunrise/sunset computation cares about, so a stricter step
constraint produces no real benefit; a free `step="any"` plus
display-time rounding gives the operator a clean number to read while
preserving the camera's stored value verbatim until they explicitly
change it.

**Removed / changed.**
- `app/html/location.html` — both lat and lon inputs change from
  `step="0.000001"` to `step="any"`.
- `app/html/js/location.js` — read-back values are rendered with
  `.toFixed(6)` before being placed in the form.
- New FR-1.6 in [01-geo-location.md](./01-geo-location.md) codifies
  the behavior.
- No change to `app/src/main.c`, `app/src/timers.c`, or
  `app/src/astro/solar.c` — rounding stays UI-only per the
  "on read only" scope chosen during requirements review.

**References.** [01-geo-location.md](./01-geo-location.md) FR-1.6,
[11-configuration-ui.md](./11-configuration-ui.md).
