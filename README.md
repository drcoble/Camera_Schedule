# Camera_Schedule

On-camera ACAP application that translates an Axis camera's geographic location
and the current date into computed solar, lunar, and seasonal event times, then
publishes those times into the camera's Event Schedules so existing camera
action rules (recording, day/night, IR illumination, PTZ guard tours, MQTT
publish, etc.) can fire on those events.

The current release is `v1.0.0-beta`. The application is feature-complete and
lab-verified on AXIS OS 11.11+ and OS 12.x running on Artpec-7 / armv7hf.
The aarch64 build is produced by CI and attached to the release page but has
not yet been smoke-tested on Artpec-8+ hardware. The `.eap` artifacts are
unsigned for the beta tag; both gates are documented in
[DL-23](./requirements/28-decision-log.md) and lift before promotion to
`v1.0.0` GA.

**This project is open source under the [MIT License](./LICENSE).** Source,
issue tracker, CI, and release artifacts are public. See [`LICENSE`](./LICENSE),
[`CONTRIBUTING.md`](./CONTRIBUTING.md), [`CODE_OF_CONDUCT.md`](./CODE_OF_CONDUCT.md),
[`SECURITY.md`](./SECURITY.md), and [`CHANGELOG.md`](./CHANGELOG.md); details in
[`requirements/25-licensing-and-distribution.md`](./requirements/25-licensing-and-distribution.md).

## Why this exists

AXIS OS exposes **no native sunrise/sunset trigger**. The DayNight API switches
the IR-cut filter on ambient lux only, and Event Schedules support fixed
time-of-day RRULEs but cannot recompute against location/date. The only prior
art is a third-party "Daybreak Me" ACAP app, which is unmaintained and does
not target ACAP v12.

## Scope at a glance

**In scope**
- Run as an ACAP application installed directly on Axis cameras.
  Artifacts are unsigned for the `v1.0.0-beta` release; signing is
  reopened post-beta when the project's Axis Application Signing key
  is in hand (see [DL-23](./requirements/28-decision-log.md)).
- Read the camera's configured geo-location and timezone from the camera itself.
- Compute solar, lunar, and seasonal event times for the camera's location/date.
- Allow the operator to define named **schedule anchors** combining an event,
  an optional offset, and a recurrence policy.
- Publish those times as **ACAP event topics** registered with the
  device's event engine, so operators bind their existing camera action
  rules to anchors via the standard Action Rules picker
  ([DL-05](./requirements/28-decision-log.md)). The original draft
  generated RFC 5545 iCalendar payloads against the Event Schedule REST
  API; that API does not exist on shipping firmware
  ([DL-05](./requirements/28-decision-log.md)) and the approach was
  dropped before M2.
- Recompute and re-arm timer sources daily, on configuration change, and
  on location / timezone change.
- Target **AXIS OS 11.11+ and AXIS OS 12.x** from a single mainline.
  Two `.eap` artifacts: armv7hf (Artpec-7) and aarch64 (Artpec-8+).
  The aarch64 build is verified by CI but not lab-tested for
  `v1.0.0-beta` — Artpec-8+ hardware smoke is a gate on promoting
  to `v1.0.0` GA (DL-23).
- Conform to ACAP v12 sandboxing, dynamic-user, and D-Bus
  credential requirements uniformly across both supported OS lines.
  Application signing is the long-term posture (NFR-5/BR-7) but
  deferred for the beta tag.

**Out of scope**
- Bundled holiday/locale database (Easter, Ramadan, etc.). Calendar input is
  user-defined dates and date-ranges only.
- Driving recordings, action rules, or guard tours **directly** — the app
  exposes schedules; the operator wires them up in the existing camera
  action-rule UI.
- Cloud sync, multi-camera fleet orchestration, or a central UI. Configuration
  is per-camera.
- GPS hardware integration (camera's stored lat/lon is authoritative).
- Overriding the lux-based DayNight API.
- Models predating armv7hf / aarch64 SoCs covered by ACAP v11/v12.

## Roadmap

The milestone-driven implementation plan is in
[`IMPLEMENTATION.md`](./IMPLEMENTATION.md) — nine milestones (M0…M8)
each ending in a tagged release. M0–M7 are shipped through `v0.7.0`.
**M8 — beta release readiness — is currently in flight** and produces
`v1.0.0-beta`: license-audit CI gate, reproducible-build verification,
`CONTRIBUTING.md` / `SECURITY.md` / `CHANGELOG.md` polish, and a public
release page with both `.eap` artifacts. See
[`CHANGELOG.md`](./CHANGELOG.md) for the per-tag history. Promotion
from beta to GA (`v1.0.0`) is gated on Application Signing CI
integration and Artpec-8+ hardware smoke per
[DL-23](./requirements/28-decision-log.md).

## Requirements layout

Detailed requirements live under [`requirements/`](./requirements):

### Functional

| File | Area |
|---|---|
| [01-geo-location.md](./requirements/01-geo-location.md) | Reading camera lat/lon, manual override |
| [02-time-and-timezone.md](./requirements/02-time-and-timezone.md) | Camera clock, IANA timezone, DST |
| [03-solar-events.md](./requirements/03-solar-events.md) | Sunrise, sunset, solar noon/midnight, twilights |
| [04-lunar-events.md](./requirements/04-lunar-events.md) | Moonrise/set, lunar transit, phases, illumination |
| [05-seasonal-events.md](./requirements/05-seasonal-events.md) | Solstices, equinoxes, longest/shortest day |
| [06-user-calendar-dates.md](./requirements/06-user-calendar-dates.md) | Operator-defined dates and date ranges |
| [07-schedule-anchors.md](./requirements/07-schedule-anchors.md) | Anchor primitive: event + offset + duration |
| [08-event-registration.md](./requirements/08-event-registration.md) | Declare ACAP event topics at boot via the AXEvent API |
| [09-event-firing.md](./requirements/09-event-firing.md) | GLib timer sources fire registered topics at the right local times |
| [10-recompute-cadence.md](./requirements/10-recompute-cadence.md) | Daily recompute, change-driven recompute, manual trigger |
| [11-configuration-ui.md](./requirements/11-configuration-ui.md) | In-camera web UI for configuration and status |
| [12-configuration-persistence.md](./requirements/12-configuration-persistence.md) | localdata JSON, AXParameter, export/import |
| [13-logging.md](./requirements/13-logging.md) | Syslog levels, in-UI status ring buffer |

### Cross-cutting

| File | Area |
|---|---|
| [20-non-functional.md](./requirements/20-non-functional.md) | Footprint, CPU, flash wear, security, signing, license posture |
| [21-platform-compatibility.md](./requirements/21-platform-compatibility.md) | AXIS OS 11.11+ floor, manifest schema, two-artifact build (armv7hf + aarch64) |
| [22-build-and-packaging.md](./requirements/22-build-and-packaging.md) | Language choice, vendored astronomy code, CI |
| [23-verification.md](./requirements/23-verification.md) | End-to-end acceptance checks |
| [24-open-questions.md](./requirements/24-open-questions.md) | Unresolved items to validate during implementation |
| [25-licensing-and-distribution.md](./requirements/25-licensing-and-distribution.md) | Project license, source availability, contribution model, public releases |
| [26-discovered-environment.md](./requirements/26-discovered-environment.md) | Probe results from lab cameras; corrections to assumptions in earlier docs |
| [27-reuse-from-timelapse2.md](./requirements/27-reuse-from-timelapse2.md) | What we lift from Fred Juhlin's MIT-licensed Timelapse2 ACAP |
| [28-decision-log.md](./requirements/28-decision-log.md) | Append-only log of substantive design decisions and removed requirements |
