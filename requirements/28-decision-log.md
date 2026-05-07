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

Date: 2026-05-05  |  Status: superseded by DL-23

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

---

## DL-18 — Schedule enable/disable: declared-but-suppressed firing model

Date: 2026-05-05  |  Status: accepted

**Decision.** Enabling and disabling individual schedules (built-in
and operator-defined) is controlled entirely on the **firing path**,
not the registration path. A disabled schedule's ACAP event topic
remains declared via `ax_event_handler_declare`; only the GLib timer
arming step in the recompute pipeline is skipped. Enable/disable state
is persisted in a flat JSON override file at
`localdata/schedule_enabled.json`, keyed by schedule ID, with the
"absent key = enabled" default. All schedules — built-in and
operator-defined — default to enabled on first use. The Schedule list
view in the configuration UI groups schedules into collapsible category
sections with a per-section enabled/total count; individual toggle
writes are committed immediately on click without a page-level Save
action; the backend endpoint is admin-gated per
[DL-13](./28-decision-log.md).

**Rationale.** Three constraints shaped the declared-but-suppressed
model:

1. **Camera Action-Rules UI stability.** The camera's Action Rules UI
   enumerates topics from the AXEvent declaration table. If a disabled
   topic is undeclared, any action rule the operator has already bound
   to it becomes a dangling reference — visible in the camera's own
   UI as a broken rule. That breakage is worse than a topic that
   silently never fires.
2. **Reservation of undeclare for structural changes.** [FR-8.5](./08-event-registration.md)
   already reserves `ax_event_handler_undeclare` for operator-driven
   add / remove / rename. A user's intent to *temporarily suppress
   firing* is a distinct operation that should not disrupt the
   registration state.
3. **Implementation simplicity.** The fire-suppression model is a
   single `if (!enabled) continue;` guard in `arm_event_slot`
   (`app/src/timers.c`). It requires zero AXEvent API calls, no
   re-declare ordering concerns at boot, and no special handling for
   pulse vs. stateful topics.

The flat JSON override file (rather than co-mingling enabled state
with `app/settings/events.json`) keeps the build-time declarative list
clean and upgrade-safe. The override file lives in `localdata/` per
[FR-12](./12-configuration-persistence.md) and survives firmware /
`.eap` upgrade.

**Removed / changed.**

- [FR-11](./11-configuration-ui.md) gains two new top-level clauses:
  - **FR-11.6 — Schedule list view** — unified list across built-ins
    and user-defined entries, collapsible category sections, search
    field, per-row layout, inline edit/delete for user-defined rows,
    immediate-write toggle semantics.
  - **FR-11.7 — Schedule enable/disable semantics** — defines the
    `localdata/schedule_enabled.json` store, the default-enabled
    rule, the firing-suppress / declaration-preserve contract, and
    the orphan-key + survival rules. (The deliverable from the UI
    review used a finer-grained sub-numbering — FR-11.2-a..e plus
    FR-11.6 — but the file's existing structure flattens these into
    FR-11.6 / FR-11.7.)
- [FR-7.2](./07-schedule-anchors.md) gains **FR-7.2-a** — built-in
  anchors are individually disable-able from the Schedule list view,
  remain non-deletable, and have their disable state preserved
  across reboot and `.eap` upgrade via the FR-11.7 store.
- [FR-8](./08-event-registration.md) gains **FR-8.8** — codifies that
  the recompute pipeline gates timer arming on the enable-state store
  while leaving the registration path untouched.
- No changes to FR-8.4, FR-8.5, or FR-8.6 — the registration path is
  unaffected by this decision.

**References.** [FR-7.2](./07-schedule-anchors.md),
[FR-8.5](./08-event-registration.md),
[FR-8.8](./08-event-registration.md),
[FR-9.2](./09-event-firing.md),
[FR-11.6 / FR-11.7](./11-configuration-ui.md),
[FR-12](./12-configuration-persistence.md),
[DL-05](./28-decision-log.md) (Path A event topics),
[DL-13](./28-decision-log.md) (admin-gated writes).

---

## DL-19 — Seasonal-event accuracy claim scoped to 1900–2050

Date: 2026-05-06  |  Status: accepted

**Decision.** [FR-5.1](./05-seasonal-events.md)'s implementation budgets
±60 s for years **1900–2050** only. Beyond 2050 the closed-form Meeus
ch. 27 routine in `app/src/astro/seasonal.c` falls back to the
Espenak-Meeus long-term ΔT parabolic and is best-effort. The host
test suite (`app/test/host/test_seasonal.c`) does not include 2100
fixtures.

**Rationale.** ΔT (TT − UT) is a *predicted* quantity past ~2025.
Established prediction models — IERS Bulletin A long-term, NASA
Eclipse pages (Espenak-Meeus 2007), and various ad-hoc IERS
extrapolations — diverge by ~50–100 s at year 2100. USNO publishes
seasons computed against a model that yields ΔT(2100) ≈ 100 s; the
Espenak-Meeus 2050–2150 long-term parabolic returns ΔT(2100) ≈ 203 s.
At ~100 s of disagreement the test would compare ΔT models, not the
ch. 27 polynomial; hand-tuning ΔT to match USNO's curve would couple
the test suite to USNO's prediction model, with zero astronomical
value. The principled long-term Espenak-Meeus polynomial is retained
in seasonal.c as a graceful fallback so callers asking for events
past 2050 still get a single-second-rounded answer (best-effort, not
guaranteed accurate).

**Removed / changed.**
- FR-5 implementation note clarifies the ±60 s budget covers the
  1900–2050 well-predicted ΔT range; 2050+ is best-effort.
- `seasonal.h` accuracy block updated to call out the 2050 horizon.
- `seasonal.c` `delta_t_seconds` 2050–2150 branch documented as a
  best-effort prediction with tens-of-seconds uncertainty.

**References.** [FR-5.1](./05-seasonal-events.md),
[FR-3.7](./03-solar-events.md) (sibling ±60 s budget),
[seasonal.c](../app/src/astro/seasonal.c),
[test_seasonal.c](../app/test/host/test_seasonal.c).

---

## DL-20 — Export/import atomicity check deferred to manual lab step

Date: 2026-05-06  |  Status: accepted

**Decision.** The host fixture `app/test/host/test_export_import.c`
does NOT simulate mid-write EIO in automated tests. The atomicity
guarantee from FR-12.1 (write-temp + fsync + parse-back + schema-validate
+ rename) is verified by a manual lab step documented in `docs/soak.md §3`.

**Rationale.** Host-side EIO injection requires intercepting the C
`write()` syscall (typically via `LD_PRELOAD` or kernel fault injection).
Neither mechanism is available in a pure-C host fixture that links only
against cJSON. Introducing a test framework (criterion, cmocka, etc.) or
a build-system dependency on `LD_PRELOAD` fixtures would add complexity
disproportionate to the verification value, since `persistence.c`'s
atomic-write helper is already unit-exercised through the M6 anchors and
calendar fixtures (which call into the same helper). The soak's filesystem
power-cycle test provides direct hardware evidence.

**Removed / changed.**
- `test_export_import.c §4` is documented as an informational-only note,
  not an executable assertion.

**References.** [FR-12.1](./12-configuration-persistence.md),
[docs/soak.md](../docs/soak.md),
[test_export_import.c](../app/test/host/test_export_import.c).

---

## DL-21 — OQ-15: RSS of camera_schedule in soak harness via /status.rss_kb

Date: 2026-05-06  |  Status: accepted

**Decision.** The SSE SHALL add a `rss_kb` integer field to the
`GET /status` JSON response (§1.1 of M7_API_CONTRACT.md). The field is
populated by reading `/proc/self/status` inside the status endpoint
handler and parsing the `VmRSS` line. This is the RSS sampling path for
the M7 soak harness.

**Rationale.** The soak harness considered SSH as an alternative (`ssh
root@camera 'cat /proc/<pid>/status'`), but SSH is typically disabled by
default on Axis firmware (BatchMode SSH to the lab cameras was not
verified at M7 STE time due to environment constraints; the harness was
written to probe and fall through rather than assume). The `system_log.cgi`
endpoint and VAPIX `param.cgi` expose neither per-process RSS nor a
mechanism to run arbitrary commands. Polling the existing `/status`
endpoint is the cleanest read-only path that works without operator
configuration changes. Adding `rss_kb` costs ~3 lines of C in the status
handler (open `/proc/self/status`, scan for `VmRSS`, close). Operators
and fleet tools may also use the field to diagnose in-the-field memory
concerns.

**Removed / changed.**
- `GET /status` response gains an optional `rss_kb` field (integer, kB).
  Absent or 0 means the platform could not read VmRSS (e.g. sandboxed
  `/proc` access denied), which is non-fatal for the soak harness.
- `M7_API_CONTRACT.md §1.1` should be updated by the SSE to document
  the `rss_kb` field in the status envelope.
- OQ-15 resolved.

**References.** [M7_API_CONTRACT.md §1.1](../app/src/M7_API_CONTRACT.md),
[docs/soak.md](../docs/soak.md),
[24-open-questions.md](./24-open-questions.md) OQ-15.

---

## DL-22 — OQ-16: M7 status endpoint renamed `/status` → `/state`

Date: 2026-05-06  |  Status: accepted

**Decision.** The M7 status panel endpoint is renamed from `/status`
to `/state` in the manifest, the C registration, the UI fetch, the
soak harness, the M7_API_CONTRACT.md, and the soak.md doc.

**Rationale.** The vendored Timelapse2 ACAP framework
(`app/src/acap/ACAP.c:691`) registers its own `/status` HTTP node
inside `ACAP()` init, intended to expose the framework's
`status_container`. The M7 SSE registered our richer status handler
under the same name. `ACAP_HTTP_Node()` (line 351) checks for
duplicate paths and returns 0 with a `LOG_WARN("Duplicate HTTP node
path: %s")` — it does NOT replace the prior registration. The
warning is easy to miss in syslog. Lab smoke-test caught the
symptom: every `GET /status` returned HTTP 200 with body `{}`,
which is `cJSON_Print` of the framework's empty initial
`status_container`. Renaming to `/state` is the smallest change
that avoids the collision while leaving the framework's `/status`
intact for future use. The framework's `/status` continues to serve
`{}` until something populates `status_container` via
`ACAP_STATUS()` — out of scope for M7.

**Removed / changed.**
- `app/manifest.json` httpConfig: `name: "status"` → `name: "state"`.
- `app/src/main.c`: `ACAP_HTTP_Node("status", ...)` → `("state", ...)`.
  Endpoint function name kept as `HTTP_Endpoint_Status` — the
  internal C identifier is fine; only the URL path changed.
- `app/html/js/schedule.js`: `fetch("status", ...)` → `fetch("state", ...)`.
- `app/test/lab/soak_common.sh`: every `/local/camera_schedule/status`
  URL → `/local/camera_schedule/state`.
- `app/src/M7_API_CONTRACT.md §1.1`: endpoint heading and references.
- `docs/soak.md`: endpoint references.
- OQ-16 resolved.

**Forward implication.** Anyone vendoring Timelapse2's ACAP
framework into a new ACAP project should treat `/app`, `/settings`,
and `/status` as **reserved** by the framework. M8's audit step
should grep for these names in any new endpoint registrations as a
pre-flight check.

**References.** [M7_API_CONTRACT.md §1.1](../app/src/M7_API_CONTRACT.md),
[ACAP.c:691](../app/src/acap/ACAP.c),
[24-open-questions.md](./24-open-questions.md) OQ-16.

---

## DL-23 — v1.0 release scoped as `v1.0.0-beta`: drop signing + Artpec-8+ lab gate from M8 acceptance

Date: 2026-05-06  |  Status: accepted

**Decision.** The first 1.x release tag is `v1.0.0-beta` (semver
pre-release suffix), not `v1.0.0`. M8's acceptance gate is
correspondingly trimmed:

- **Signing is deferred.** The release pipeline SHALL continue to
  produce both unsigned `.eap` artifacts (armv7hf + aarch64) via the
  same CI matrix that ships v0.7.0. Axis Application Signing
  integration (formerly required by **BR-7** and **NFR-5**) moves
  out of M8 scope and into a future milestone, to be reopened when
  the signing key is in hand or when ACAP v12 makes signing
  mandatory in shipping firmware (whichever comes first).
- **Artpec-8+ (aarch64) lab smoke is dropped** from M8 acceptance.
  The aarch64 `.eap` SHALL still be built by CI on every change
  (the build matrix is unchanged) and SHALL still be attached to
  the v1.0.0-beta release page, but its "smoke-test on real
  Artpec-8+ hardware" line item is deferred until an Artpec-8+ unit
  joins the lab. Both lab cameras remain Artpec-7 / armv7hf.
- **Public release page deliverable kept**, just unsigned. v1.0.0-beta
  ships GitHub Releases with both `.eap` files, SHA-256 checksums,
  `THIRD_PARTY_LICENSES.md`, `CHANGELOG.md`. Operators downloading
  the unsigned artifact accept that uninstalling and reinstalling
  may require additional approval steps once Axis enforces signing.
- **Other M8 deliverables stand**: license-audit CI gate (NFR-6,
  DR-11), reproducible-build verification (BR-6),
  `CONTRIBUTING.md` / `SECURITY.md` / `CHANGELOG.md` polish, full
  acceptance suite from `23-verification.md` on the two armv7hf
  lab cameras.

**Rationale.** Three constraints converged. (1) The Axis Application
Signing key has not been obtained; bringing it in is a procurement
step external to the codebase, with no fixed timeline. (2) The lab
has no Artpec-8+ hardware; CI builds the aarch64 artifact and the
unit tests are arch-independent (host-side `make test` covers all
algorithmic correctness; the only thing missing is install-and-run
on real aarch64 silicon). (3) The work that is feature-complete and
already lab-verified (M0-M7) plus the M8 polish that doesn't depend
on (1) or (2) constitutes a usable beta — gating it on procurement
+ hardware acquisition would freeze a release that already provides
operator value. Tagging as `v1.0.0-beta` signals "API surface
stabilized, production-ready on armv7hf, awaiting signing key and
aarch64 hardware verification before promotion to `v1.0.0`."

**Removed / changed.**
- **DL-11** marked superseded. Signing workflow as described still
  applies *when implemented* — the secret-store / tag-only / no-PR
  pattern is preserved for the future signing rollout — but is no
  longer a v1.0 acceptance gate.
- **NFR-5** amended: signing remains the long-term release posture
  but is no longer mandatory for v1.0.0-beta. The "shipping signed
  from day one" stance is renumbered as a post-beta goal.
- **BR-7** amended: signing pipeline language stays in the spec as
  the target end state; the SHALL clause is downgraded to MAY for
  v1.0.0-beta.
- **DR-12** amended: tagged-release artifacts MAY be unsigned for
  v1.0.0-beta; SHALL be signed for v1.0.0 GA and later.
- **PR-3 / PR-6** unchanged: the build matrix still produces both
  artifacts; aarch64 lab smoke moves from "required for v1" to a
  post-beta gating step.
- **23-verification.md** acceptance check #1 amended: "two signed
  `.eap` files" → "two `.eap` files (unsigned for v1.0.0-beta)".
- **23-verification.md** "aarch64 coverage requires Artpec-8+ unit
  before release" amended to "before promotion to v1.0.0 GA".
- **IMPLEMENTATION.md M8** rewritten to drop signing + aarch64
  lab-test from deliverables; tag changes from `v1.0.0` to
  `v1.0.0-beta`. The signing CI integration and Artpec-8+
  acquisition are added to "deliberately deferred past v1.0".

**Tag-naming convention.** `v1.0.0-beta` is the canonical
semver-2.0 pre-release suffix and sorts correctly before
`v1.0.0` in `git tag --sort=v:refname` (with `--sort=-v:refname`
git treats `1.0.0` as newer than `1.0.0-beta`, which is what we
want).

**References.** [22-build-and-packaging.md](./22-build-and-packaging.md)
BR-7, [21-platform-compatibility.md](./21-platform-compatibility.md)
PR-3 / PR-6, [20-non-functional.md](./20-non-functional.md) NFR-5,
[25-licensing-and-distribution.md](./25-licensing-and-distribution.md)
DR-12, [23-verification.md](./23-verification.md),
[../IMPLEMENTATION.md](../IMPLEMENTATION.md) M8, DL-11.

---

## DL-24 — M8 docs polish: Keep-a-Changelog 1.1.0 + DCO mandatory

Date: 2026-05-06  |  Status: accepted

**Decision.** Two M8 documentation conventions are locked:

1. **`CHANGELOG.md` follows [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/)**
   with semver section headings. Sections per release are limited to
   **Added**, **Changed**, **Fixed**, **Removed** (the four most
   commonly populated of the six Keep-a-Changelog defaults; the
   "Deprecated" and "Security" sections are omitted from the
   per-release scaffolding and may be added ad-hoc when populated).
   Tag-named anchor links at the bottom of the file point at
   `https://github.com/drcoble/Camera_Schedule/releases/tag/<tag>`.
   Releases not actually tagged in the repo (e.g. v0.4.0 and v0.5.0,
   whose work shipped inside v0.6.0 per
   [CLAUDE.md](../CLAUDE.md) and the M4/M5 calendar-locked-tag
   policy) do not get a CHANGELOG section.
2. **DCO sign-off is mandatory, not recommended.** [DR-7](./25-licensing-and-distribution.md)
   describes DCO sign-off as "recommended"; M8's `CONTRIBUTING.md`
   tightens this to a hard policy — every contributor commit MUST
   carry a `Signed-off-by:` trailer matching the author's
   `git config user.name` and `user.email`. Enforcement is by
   reviewer policy for v1.0.0-beta; promoting to a CI check is
   tracked separately and is not a v1.0.0-beta gate.

**Rationale.** Keep-a-Changelog is the de-facto standard for
human-readable changelogs in MIT-licensed open-source projects;
adopting v1.1.0 (the current spec at the time of M8) gives
contributors a recognizable structure and avoids inventing a
project-specific format. Restricting per-release sections to the
four most-used categories keeps the file scannable — empty section
headers signal nothing useful and add visual clutter.

Tightening DCO from "recommended" to "mandatory" matches how every
substantive open-source ACAP project the maintainer surveyed
operates and removes ambiguity about whether unsigned PRs need to
be re-submitted. The change is procedural, not legal — DCO has been
the project's intended posture from DL-01, and no shipped commit
history is retroactively affected (initial commits pre-date the
policy).

**Removed / changed.**

- New file `CHANGELOG.md` at repo root, conformant to Keep-a-Changelog 1.1.0.
- `CONTRIBUTING.md` § Developer Certificate of Origin (DCO) states
  the policy as mandatory and shows the `git commit -s` invocation.
- `DR-7`'s "recommended: `Signed-off-by:` per the Developer
  Certificate of Origin v1.1" is interpreted by `CONTRIBUTING.md`
  as a hard requirement; the requirements file itself is not
  edited (DR-7 wording stands; the project's interpretation is
  recorded here so future contributors who read DR-7 see the
  pointer to DL-24).

**References.** [DR-7 / DR-13](./25-licensing-and-distribution.md),
[../CONTRIBUTING.md](../CONTRIBUTING.md),
[../CHANGELOG.md](../CHANGELOG.md),
[../IMPLEMENTATION.md](../IMPLEMENTATION.md) M8.

---

## DL-25 — License-audit gate uses an in-tree Python script + SPDX header scan

Date: 2026-05-06  |  Status: accepted

**Decision.** The M8 license-audit gate
([NFR-6](./20-non-functional.md), [DR-11](./25-licensing-and-distribution.md))
is implemented as a hand-rolled Python script
(`app/scripts/license_audit.py`, ~250 LOC, stdlib-only) plus a flat
`approved-licenses.txt` allowlist at the repo root. The script walks
the .eap-bundled file tree, detects each file's SPDX identifier from
its header (project-owned C/headers), classifies vendored
third-party files via an in-script manifest cross-checked against
`THIRD_PARTY_LICENSES.md`, and fails non-zero if any classified
license falls off the allowlist or any file under `app/src/` /
`app/settings/` is unclassifiable. The script runs as its own CI job
(`.github/workflows/license-audit.yml`) on every push and PR.

Three off-the-shelf alternatives were considered and rejected:

- **`reuse` (FSF, SPDX-aware).** Idiomatic for SPDX-headered
  projects, but enforces SPDX headers on *every* file in the tree.
  The repo's HTML/CSS/JS UI assets were authored without SPDX
  headers; adopting `reuse` would force expanding header coverage
  across `app/html/` — outside M8 scope and outside SSE-A's edit
  boundary (UI files belong to other active workstreams).
- **`licensecheck` (Debian).** Looks at file content for license
  *text*, not SPDX. Works without headers but its output is
  unstructured ("UNKNOWN", "MIT/X11", various capitalizations) and
  wrapping it to compare against `approved-licenses.txt` ends up
  about the same size as the audit script we wrote — without the
  inventory cross-check.
- **`scancode-toolkit`.** Most thorough; pulls a multi-package pip
  dependency tree and runs for ~30 s on a small project. Overkill
  for a tree of ~36 bundled files and adds CI surface for no gain
  at this size.

The hand-rolled script is ~250 LOC of stdlib Python with no pip
install step, exits 1 on policy violation, and emits a markdown
audit report uploaded as a CI artifact. It catches the three failure
modes the gate cares about: (1) a new file with a non-allowlist
SPDX header (e.g. an LGPL drop into `app/src/`), (2) an
undocumented vendored file (no SPDX header, not in
`VENDORED_FILES`), (3) inventory drift between
`THIRD_PARTY_LICENSES.md` and the file tree.

**Approved-license set** (committed as `approved-licenses.txt`):
MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, ISC, 0BSD,
Unlicense, CC0-1.0. LGPL is *not* on the list — adding it requires
the `lgpl-relink/` release-pipeline work per NFR-6 and a new DL
entry justifying the relink commitment.

**Removed / changed.**
- `THIRD_PARTY_LICENSES.md` gains a top-level pointer to the audit
  workflow + allowlist file.
- `app/scripts/license_audit.py` is the canonical implementation;
  any future rewrite (e.g. switching to `reuse` once UI SPDX
  headers ship) must preserve the three failure modes above.

**References.** [NFR-6](./20-non-functional.md),
[DR-10/DR-11](./25-licensing-and-distribution.md),
[../THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md),
[../approved-licenses.txt](../approved-licenses.txt),
[../app/scripts/license_audit.py](../app/scripts/license_audit.py),
[../.github/workflows/license-audit.yml](../.github/workflows/license-audit.yml).

---

## DL-26 — Reproducibility plumbing: SOURCE_DATE_EPOCH + post-build .eap repack + opt-in SDK digest pin

Date: 2026-05-06  |  Status: accepted

**Decision.** [BR-6](./22-build-and-packaging.md) reproducibility is
enforced by three independent pieces of plumbing, all sitting outside
the in-container `acap-build` invocation (`acap-build` itself is a
black box and was empirically not assumed to honor
`SOURCE_DATE_EPOCH`):

1. **`SOURCE_DATE_EPOCH` defaulted from the head commit.** The
   Makefile sets `SOURCE_DATE_EPOCH ?= $(git log -1 --pretty=%ct)`
   so a clean checkout at a given SHA derives the same epoch
   without operator action. CI exports the same value before
   invoking `make`.
2. **`LC_ALL=C` exported by the Makefile.** Pins `sort` and other
   locale-sensitive utilities to the C collation, eliminating a
   second-order non-determinism source.
3. **`app/scripts/repack_eap.sh` rewrites the `.eap` after
   `acap-build`.** The `.eap` is a gzip'd tar; the repack
   re-extracts, normalizes file mtimes to `SOURCE_DATE_EPOCH`,
   re-tars with `--sort=name --owner=0 --group=0 --numeric-owner
   --mtime=@SOURCE_DATE_EPOCH --format=ustar`, and recompresses with
   `gzip -n9`. `gzip -n` strips the original-filename and mtime
   fields per RFC 1952 §2.3.1, eliminating the embedded build-time
   timestamps. The Makefile invokes the repack from
   `build-armv7hf` and `build-aarch64`, so the deterministic `.eap`
   is the only artifact produced.

The compiled ELF binary inside the .eap is reproducible *given* the
SDK image is bit-stable across the two runs:

- Linker flag `-s` (already present in `LDLIBS` since v0.1.0)
  strips the binary at link time, removing DWARF debug-info
  timestamps and randomized ELF build-ids.
- The project sources do not use `__DATE__` / `__TIME__` /
  `__FILE__`-leaking-an-absolute-path patterns (verified by
  grepping `app/src/`).

**SDK image digest pin.** The Dockerfile accepts an optional
`SDK_DIGEST` build-arg. When non-empty (e.g. `--build-arg
SDK_DIGEST=sha256:abc...`), the `FROM` line resolves to
`axisecp/acap-native-sdk:<tag>@<digest>`, pinning the image
immutably. When empty (default for dev / CI builds), it falls
back to the floating tag — acceptable because both runs of the
reproducibility CI job use the same already-pulled local image
within a single workflow run.

For releases, the digest is resolved automatically by
`.github/workflows/release.yml` via `docker buildx imagetools
inspect ...` and threaded into the build through the per-arch
Makefile vars `SDK_DIGEST_armv7hf` / `SDK_DIGEST_aarch64`
(per-arch because the SDK image is multi-platform and each arch
has its own digest). This keeps the dev workflow ergonomic — a
plain `make build-armv7hf` keeps working — while making the
release artifact's SDK provenance verifiable.

The integrator can still pin manually for ad-hoc reproducibility
checks:

```
SDK_DIGEST_armv7hf=sha256:abc... make -C app build-armv7hf
```

**Verification gate.** A new CI workflow,
`.github/workflows/reproducibility.yml`, builds each architecture
twice on every push/PR and asserts SHA-256 identity. On mismatch
it diffs tar contents and uploads both `.eap` files as forensic
artifacts.

**Removed / changed.**
- Dockerfile's prior comment "before tagging v1.0 (M8), the
  production tags below should be replaced with @sha256:... pins"
  is implemented as the `SDK_DIGEST` build-arg path above.
- Makefile gains `SOURCE_DATE_EPOCH`, `LC_ALL=C`, and the
  `REPACK_EAP` invocation in the build-armv7hf / build-aarch64
  recipes.
- Makefile gains a new `reproducibility-check` target for local
  verification (CI uses the workflow).

**References.** [BR-6](./22-build-and-packaging.md),
[../app/Dockerfile](../app/Dockerfile),
[../app/Makefile](../app/Makefile),
[../app/scripts/repack_eap.sh](../app/scripts/repack_eap.sh),
[../.github/workflows/reproducibility.yml](../.github/workflows/reproducibility.yml).

---

## DL-27 — Release workflow always creates a *draft* release; integrator publishes manually

Date: 2026-05-06  |  Status: accepted

**Decision.** The release workflow
(`.github/workflows/release.yml`) creates GitHub Releases with the
`--draft` flag unconditionally. Promotion from draft to public is
a manual integrator step (GitHub UI or
`gh release edit <tag> --draft=false`).

**Rationale.** [DR-12](./25-licensing-and-distribution.md) requires
a public release page on tag, but says nothing about whether
publication should be automatic. Three reasons to gate publication
on a human:

1. **Asset review.** The release attaches both `.eap` files,
   `SHA-256SUMS.txt`, `THIRD_PARTY_LICENSES.md`, and
   `CHANGELOG.md`. A manual review confirms the assembled set
   is what the integrator expected before the world sees it.
   Beta releases especially benefit — the deferred-signing /
   deferred-aarch64-smoke posture from
   [DL-23](./28-decision-log.md) needs a clearly worded
   release-page note that's easier to author by hand than to
   template.
2. **CHANGELOG synchronization.** SSE-B owns CHANGELOG.md and
   may push final wording in a separate commit late in the cycle.
   A draft lets the integrator regenerate the release page (delete
   draft, re-tag if needed, or `gh release edit --notes-file`)
   without an awkward "republished" announcement.
3. **Mistake recovery.** A tag pushed by mistake produces a
   draft, not a public release. The draft can be discarded
   without leaving a permanent artifact in the public release
   list.

The release notes body is auto-extracted from the `CHANGELOG.md`
section matching the tag (Keep a Changelog convention:
`## [vX.Y.Z]` or `## vX.Y.Z`). If the section is missing, the
body falls back to a one-line "re-run after CHANGELOG lands"
placeholder so the workflow doesn't fail; the integrator
amends the draft body before publishing.

**Pre-release flag.** Tags ending in `-beta` get
`--prerelease`; everything else is a full release once
published.

**Dry-run path.** A `workflow_dispatch` trigger with
`dry_run=true` (default) runs the build + assemble path without
calling `gh release create`. Used by the integrator to verify
the assembly works before pushing the actual tag.

**Removed / changed.**
- Release-page deliverable from DR-12 / DL-23 is now the draft
  output of `release.yml`; the human "publish" step is the
  documented hand-off.

**References.** [DR-12](./25-licensing-and-distribution.md),
[DL-23](./28-decision-log.md),
[../.github/workflows/release.yml](../.github/workflows/release.yml).
