# Non-functional requirements

Cross-cutting constraints on size, performance, security, and licensing.

## Requirements

- **NFR-1 — Footprint.** The installed `.eap` SHALL be ≤ **5 MB**. Peak
  resident set size SHALL be ≤ **32 MB** during a recompute.

- **NFR-2 — CPU.** A full recompute (90-day look-ahead, all anchors, all
  event types) SHALL complete in ≤ **2 s** wall time on a current-generation
  ARTPEC SoC. Daily steady-state CPU SHALL be negligible (idle most of the
  day, brief recompute peaks).

- **NFR-3 — Flash wear.** Configuration writes SHALL be coalesced
  ([FR-12.5](./12-configuration-persistence.md)). No file in `localdata/`
  SHALL be rewritten more than once per minute except on explicit user save.
  No log rotation or cache file SHALL write to flash on a continuous basis.

- **NFR-4 — Sandboxing.** The app SHALL run as a **dynamic non-root
  user** on every supported OS target (AXIS OS 11.11+ and 12.x). The
  manifest SHALL declare only the D-Bus methods actually used
  (currently: `com.axis.HTTPConf1.VAPIXServiceAccounts1.GetCredentials`).
  No outbound internet access is required and none SHALL be requested.
  The app SHALL pass an `apparmor_status` 24-hour soak with zero policy
  violations on the OS 12 target ([FR-23.8](./23-verification.md)).

- **NFR-5 — Application signing.** Both `.eap` artifacts (armv7hf and
  aarch64) SHALL be signed with the project's Axis Application Signing
  key prior to release. Signing is becoming mandatory in upcoming AXIS
  OS releases; shipping signed from day one future-proofs the artifact.

- **NFR-6 — License posture.** Bundled third-party code SHALL be compatible
  with the project's open-source license
  (see [25-licensing-and-distribution.md](./25-licensing-and-distribution.md)).
  Specifically:
  - **NREL SPA** (noncommercial U.S. government license) SHALL NOT be
    bundled — the noncommercial restriction is incompatible with any
    OSI-approved license.
  - **Permissive licenses preferred** (MIT, BSD-2/3-Clause, Apache-2.0, ISC,
    public domain). These avoid relinking obligations and keep the `.eap`
    redistributable without friction.
  - **LGPL is permitted** but discouraged: ACAP `.eap` packages are
    effectively static artifacts, so honoring the LGPL relink obligation
    requires shipping object files or sources alongside each release. If
    LGPL code is bundled, the release pipeline SHALL produce the
    corresponding `lgpl-relink/` artifact.
  - **GPL is permitted only if** the project chooses GPL itself; bundled
    GPL code SHALL NOT be added under a permissive project license.
  - Vendored astronomy code SHALL be a clean-room
    MIT / BSD / public-domain C implementation of NOAA / Meeus algorithms
    (no LGPL relink obligation, no SPA license issue).

- **NFR-7 — No outbound network.** The app SHALL not initiate connections
  outside the camera (no telemetry, no update check, no analytics). All
  HTTP traffic SHALL terminate at `127.0.0.12`.

- **NFR-8 — Robustness.** The app SHALL not crash, loop, or leak on:
  invalid lat/lon, missing timezone, NTP not yet synced, VAPIX returning
  5xx, malformed JSON config on disk, or upgrade-time schema drift. Each of
  these states SHALL produce a clear UI-surfaced error.
