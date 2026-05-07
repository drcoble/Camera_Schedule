# Security policy

Camera_Schedule runs on-camera as an ACAP application under the AXIS
OS dynamic-user sandbox (no root). This document describes how to
report a vulnerability, what is in scope, and what to expect after
you file a report.

## Reporting a vulnerability

**Do not** open a public GitHub issue for a security report.

Use **GitHub private vulnerability reporting**: open a private
advisory via the repository's *Security* tab → *Report a vulnerability*
at <https://github.com/drcoble/Camera_Schedule/security/advisories/new>.

This is the only supported reporting channel. A GitHub account is
required to file an advisory; if you do not have one and cannot
create one, ask a trusted intermediary to file on your behalf.

Please include, at minimum:

- The affected version (`v1.0.0-beta` or a commit SHA).
- The platform you reproduced on (AXIS OS version, Artpec generation,
  installed firmware patch level).
- A minimal reproduction or proof-of-concept.
- The impact you observed.

## Response window

| Severity | Acknowledgement | Fix or workaround |
|---|---|---|
| CVSS ≥ 7.0 (high or critical) | within 7 days | within 30 days |
| CVSS < 7.0 | within 7 days | best-effort, in the next tagged release |

We follow coordinated disclosure: report → triage → fix → release →
public advisory. Embargoes longer than 90 days are negotiated case
by case.

## Supported versions

Only the most recent tagged release is eligible for security fixes.
The 0.x line was a pre-release sequence and is not supported.

| Version | Status |
|---|---|
| `v1.0.0-beta` | Supported |
| `0.x` (any tag) | Not supported — upgrade to `v1.0.0-beta` |

## In scope

The following are in scope for a security report:

- **Remote code execution** on the camera via any FastCGI endpoint
  the app exposes under `/local/camera_schedule/`.
- **Privilege escalation** out of the dynamic-user sandbox.
- **AppArmor policy escape** caused by the app's code paths.
- **Secret or credential exposure** — VAPIX credentials obtained via
  D-Bus, geolocation values written to the wrong destination,
  contents of `localdata/` leaked through an endpoint without
  authentication.
- **Integrity tampering of the released `.eap`** — the bundled
  `.eap` and its published SHA-256 do not match, or the build is
  not reproducible from public sources
  ([DR-5](./requirements/25-licensing-and-distribution.md)).
- **Authentication or authorization bypass** of the manifest's
  `httpConfig.access` gate
  ([DL-13](./requirements/28-decision-log.md)) — for example, an
  endpoint that should require admin credentials accepting viewer
  credentials.
- **Injection** into the camera's geolocation service or
  AXParameter store from any unauthenticated input path.

## Out of scope

The following are **not** treated as security issues. File them as
regular bugs or feature requests in the public issue tracker.

- **Schedule-time inaccuracy beyond the documented tolerances**
  ([FR-3.7](./requirements/03-solar-events.md),
  [FR-4.5](./requirements/04-lunar-events.md),
  [FR-5.1](./requirements/05-seasonal-events.md) — these are
  functional bugs.
- **Feature requests** of any kind, including requests for new
  event types, new anchor semantics, or new UI capability.
- **Denial-of-service against the on-camera UI from a logged-in
  admin user.** An admin already has full control of the camera
  (camera reboot, app uninstall, factory reset) and is not a
  threat actor in our model.
- **Denial-of-service via crafted geolocation, anchor, or calendar
  values that the input validators reject.** The validation layer
  is functioning as designed in that case.
- **Issues that require physical access to the camera** beyond the
  level of access that AXIS OS itself already grants the
  console-attached user.

## Threat model

Within the AXIS OS dynamic-user sandbox the app's attack surface is:

- The FastCGI endpoints registered under `/local/camera_schedule/`
  (eleven endpoints as of `v1.0.0-beta`: `about`, `location`,
  `anchors`, `calendar`, `events`, `events_today`, `state`,
  `recompute`, `export`, `import`, `debug`). Authentication and
  role gating are enforced by the manifest's `httpConfig.access`
  field.
- D-Bus calls to
  `com.axis.HTTPConf1.VAPIXServiceAccounts1.GetCredentials` to
  obtain VAPIX credentials for the geolocation read/write path.
- Local file I/O under the ACAP-allocated `localdata/` directory
  (anchors, calendar, schedule-enabled override, exported config).

The app does **not**:

- Open inbound network sockets of its own.
- Read or write the camera's video pipeline.
- Require root.
- Bundle a tzdata, holiday, or third-party astronomical database
  that could become a stale-data vector
  ([DL-10](./requirements/28-decision-log.md)).

## Release artifact integrity

For `v1.0.0-beta` the `.eap` artifacts are published **unsigned**
per [DL-23](./requirements/28-decision-log.md). Integrity is
verifiable two ways:

1. **SHA-256 checksums** are published alongside each `.eap` on the
   GitHub release page.
2. **Reproducible build** from the public source tree using the
   pinned ACAP Native SDK Docker image yields the identical
   `.eap` SHA-256
   ([BR-6](./requirements/22-build-and-packaging.md),
   [DR-5](./requirements/25-licensing-and-distribution.md)).

Axis Application Signing is the long-term posture and is reopened
post-beta when the project's signing key is in hand
([DL-23](./requirements/28-decision-log.md), supersedes
[DL-11](./requirements/28-decision-log.md)). When signing returns,
this section will be amended to document key custody, rotation
cadence, and the tag-only signing pipeline.
