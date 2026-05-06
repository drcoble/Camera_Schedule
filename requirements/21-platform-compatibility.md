# Platform & compatibility requirements

Camera_Schedule targets **AXIS OS 11.11 and newer** on a unified codebase
and manifest. The build pipeline produces two `.eap` artifacts to cover
both supported architectures. Cameras running AXIS OS 11.10 or earlier
are unsupported (see [DL-15](./28-decision-log.md)).

## Targets

- **PR-1 — Single platform target: AXIS OS 11.11+.**
  - ACAP Native SDK **12.x** (one SDK serves both OS targets).
  - Manifest schema **1.7.x** — accepted by both OS 11.11+ and OS 12.x.
  - `embeddedSdkVersion: "3.0"`.
  - `runMode: "respawn"`.
  - **No manifest-level OS-compatibility declaration** — earlier draft
    required `compatibleOsVersions: [{ versionRange: ... }]`, but that
    field doesn't exist in any manifest schema bundled with SDK 12.6.0
    (v1.0 → v1.8.0 all rejected). See [DL-16](./28-decision-log.md);
    OS-compatibility is enforced at runtime by the AXIS OS 11.11+ APIs
    we link against and stated explicitly in release notes.
  - Dynamic-user model uniformly. Application signing required on v12;
    same signed `.eap` is also accepted by OS 11.11+.
  - D-Bus credential acquisition for VAPIX loopback uniformly
    ([FR-1](./01-geo-location.md)).

- **PR-2 — Architectures.**
  - **armv7hf** — Artpec-7 (and earlier-still-supported) SoCs running
    AXIS OS 11.11+ or 12.x.
  - **aarch64** — Artpec-8 and newer SoCs running AXIS OS 12.x.
  - There is no aarch64 build for AXIS OS 11; OS 11 is armv7hf-only.

- **PR-3 — Build matrix: two .eap artifacts.**

  | Artifact | Architecture | Manifest schema | OS targets |
  |---|---|---|---|
  | `camera-schedule-armv7hf.eap` | armv7hf | 1.7.x | OS 11.11+ and OS 12.x on Artpec-7 |
  | `camera-schedule-aarch64.eap` | aarch64 | 1.7.x | OS 12.x on Artpec-8+ |

  Both artifacts SHALL be built from one source tree using two Docker
  SDK images (`axisecp/acap-native-sdk:12.x-armv7hf-ubuntu24.04` and
  the corresponding `aarch64` image). The manifest SHALL be a single
  `manifest.json` shared by both builds; only the `architecture` field
  differs at build time.

- **PR-4 — No root anywhere.** No code path SHALL require root
  privileges on any supported OS target. The dynamic-user + D-Bus
  credential model is the only auth pattern.

- **PR-5 — Camera-side prerequisites.** The release SHALL document
  required camera state:
  - AXIS OS **11.11.0 or newer** (cameras on 11.10 or earlier need to
    upgrade or stay on a different solution),
  - NTP enabled and converged,
  - valid camera geo-location set, **or** a manual override entered in
    the app's UI (which writes back to the camera's geolocation
    service per [FR-1.3](./01-geo-location.md)),
  - Geolocation API present (`Properties.Geolocation.Geolocation=yes`).

- **PR-6 — CI integration smoke tests.** CI SHALL include integration
  smoke tests against at least:
  - the **lowest supported AXIS OS 11.11.x** image,
  - the **latest AXIS OS 12.x** image.

  The lab OS 11.10.83 unit is *below* the supported floor and SHALL NOT
  be used as an integration test target until upgraded to OS 11.11.x.

## Notes

- The earlier draft separated v11 and v12 into distinct manifest
  schemas, SDKs, and auth models. Hardware probing
  ([26-discovered-environment.md](./26-discovered-environment.md))
  proved unnecessary: Artpec-7 cameras run AXIS OS 12 on armv7hf, the
  manifest 1.7.x schema is accepted by 11.11+, and the same `.eap`
  installs cleanly across both OS lines. The simplification is captured
  in [DL-15](./28-decision-log.md).
- ACAP v12 mandatory signing is in flight at Axis. Shipping signed from
  day one ([NFR-5](./20-non-functional.md)) future-proofs the artifact.
  Signing is deferred for `v1.0.0-beta`
  ([DL-23](./28-decision-log.md)); the build matrix still produces
  both armv7hf and aarch64 `.eap` files in the meantime.
