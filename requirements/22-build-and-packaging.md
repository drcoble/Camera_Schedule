# Build & packaging requirements

Implementation language, third-party-code policy, and CI obligations.

## Requirements

- **BR-1 — Language: ACAP Native (C99).** Implementation SHALL use the
  ACAP Native SDK in C, targeting C99. Rationale:
  - smallest footprint (well under the [NFR-1](./20-non-functional.md) 5 MB cap),
  - one SDK (12.x) covers both supported OS lines (11.11+ and 12.x),
  - license flexibility for vendored astronomy code,
  - no Python runtime dependency.

- **BR-2 — Vendored astronomy.** Celestial computation SHALL be a vendored,
  in-tree, MIT-licensed C implementation of:
  - NOAA / Meeus solar position algorithm (sunrise/sunset and zenith-N
    twilights), with polar `NO_EVENT_TODAY` sentinels,
  - Meeus *Astronomical Algorithms* ch. 47 (lunar position) and ch. 49
    (lunar phases),
  - Meeus ch. 27 (solstices and equinoxes),
  - hourly-altitude interpolation for moonrise / moonset.

  The solar core SHALL be derived from Timelapse2's `sunevents.c`
  (MIT, Fred Juhlin 2024) and refactored to be parameterized by zenith
  angle so the three twilight altitudes share one routine — see
  [27-reuse-from-timelapse2.md](./27-reuse-from-timelapse2.md). Lunar and
  seasonal modules are new code.

  Estimated size: **1000–1500 lines of C**, no external library dependency
  beyond `libm`.

- **BR-2a — ACAP framework reuse.** The HTTP/FastCGI, AXEvent, AXParameter,
  VAPIX, and `ACAP_DEVICE_*` plumbing SHALL be vendored from Timelapse2's
  `ACAP.c` / `ACAP.h` (MIT) plus `cJSON.c` / `cJSON.h` (MIT). Reuse rules,
  attribution, and modification markers per
  [27-reuse-from-timelapse2.md](./27-reuse-from-timelapse2.md).

- **BR-3 — Host-side test harness.** The repo SHALL include a host-side
  test harness (x86_64 Linux, runs in CI without an emulator) that:
  - validates computed events against USNO / NOAA reference data,
  - covers ≥ 100 lat/lon × date fixtures spanning equator → 75°,
  - includes DST spring-forward and fall-back boundaries,
  - includes polar conditions (winter / summer at 78° N and 78° S),
  - includes lunar fixtures across at least one full Metonic-cycle worth of
    new/full moons.

- **BR-4 — CI obligations.** CI SHALL on every change:
  - build both `.eap` artifacts (armv7hf + aarch64),
  - run the full host-side test suite,
  - run the manifest validator against the declared schema versions,
  - lint C with `clang-tidy` (or equivalent) at a defined ruleset,
  - reject any build whose `.eap` size exceeds the
    [NFR-1](./20-non-functional.md) cap,
  - run a license-and-attribution audit
    ([NFR-6](./20-non-functional.md), [25-licensing-and-distribution.md](./25-licensing-and-distribution.md))
    on the final artifacts.

- **BR-5 — Public CI.** CI SHALL run on a public CI surface (GitHub Actions
  on the project's public repository, or equivalent). Build logs and
  artifacts of tagged releases SHALL be publicly browsable to support
  third-party verification.

- **BR-6 — Reproducibility.** Builds SHALL be reproducible from a clean
  checkout: no network access at compile time beyond the pinned Docker SDK
  base images. The exact base image digest SHALL be pinned (not `:latest`).

- **BR-7 — Signing in release builds.** The release pipeline SHALL sign
  both `.eap` artifacts (armv7hf and aarch64) with the project's Axis
  Application Signing key ([NFR-5](./20-non-functional.md)). Development
  builds MAY skip signing. Signing keys SHALL be held in a CI secrets
  store (GitHub Actions secret); signing SHALL only occur on tagged
  releases from the protected `main` branch.

- **BR-8 — Public release artifacts.** Tagged releases SHALL publish the
  signed `armv7hf` and `aarch64` `.eap` files plus their SHA-256
  checksums and a third-party-license bundle to the project's public
  release channel (e.g. GitHub Releases). See
  [25-licensing-and-distribution.md](./25-licensing-and-distribution.md).

## Initial source layout (target)

Reflects the Path A architecture, Timelapse2 framework reuse, and the
two-artifact build matrix from [DL-15](./28-decision-log.md).

- `app/manifest.json` (single manifest, schema 1.7.x, shared across
  armv7hf and aarch64 builds)
- `app/Makefile`, `app/Dockerfile` (single Dockerfile parameterized by
  `--build-arg ARCH=armv7hf|aarch64`)
- `app/settings/events.json` — declarative event-topic list
  ([27-reuse-from-timelapse2.md](./27-reuse-from-timelapse2.md))
- `app/src/main.c` — lifecycle, GLib main loop, signal handling
- `app/src/acap/ACAP.c|h` — vendored from Timelapse2 (MIT)
- `app/src/acap/cJSON.c|h` — vendored (MIT)
- `app/src/astro/solar.c|h` — refactored from Timelapse2 + extensions
  ([FR-3](./03-solar-events.md))
- `app/src/astro/lunar.c|h` — new ([FR-4](./04-lunar-events.md))
- `app/src/astro/seasonal.c|h` — new ([FR-5](./05-seasonal-events.md))
- `app/src/anchors.c|h` — anchors → event-times → timer arming
  ([FR-7](./07-schedule-anchors.md))
- `app/src/calendar.c|h` — user calendar entries
  ([FR-6](./06-user-calendar-dates.md))
- `app/src/timers.c|h` — midnight + per-event GLib timer sources
  ([FR-10](./10-recompute-cadence.md))
- `app/src/config.c|h` — JSON config load/save in `localdata/`, atomic
  write ([FR-12](./12-configuration-persistence.md))
- `app/html/` — vanilla JS/HTML config UI + bundled vendor assets
  ([FR-11](./11-configuration-ui.md))
- `app/test/host/` — host-side test harness with USNO/NOAA fixtures (BR-3)
