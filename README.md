# Camera_Schedule

[![License: MIT](https://img.shields.io/github/license/drcoble/Camera_Schedule)](./LICENSE)
[![Latest release](https://img.shields.io/github/v/release/drcoble/Camera_Schedule?include_prereleases&sort=semver)](https://github.com/drcoble/Camera_Schedule/releases)
[![CI](https://img.shields.io/github/actions/workflow/status/drcoble/Camera_Schedule/ci.yml?branch=main&label=CI)](https://github.com/drcoble/Camera_Schedule/actions/workflows/ci.yml)
[![License audit](https://img.shields.io/github/actions/workflow/status/drcoble/Camera_Schedule/license-audit.yml?branch=main&label=license%20audit)](https://github.com/drcoble/Camera_Schedule/actions/workflows/license-audit.yml)
[![Reproducible build](https://img.shields.io/github/actions/workflow/status/drcoble/Camera_Schedule/reproducibility.yml?branch=main&label=reproducible%20build)](https://github.com/drcoble/Camera_Schedule/actions/workflows/reproducibility.yml)

An on-camera ACAP application for Axis network cameras. It turns the camera's
geographic location and the current date into computed **solar, lunar, and
seasonal event times**, then publishes those moments as **camera event topics**.
Operators bind their existing action rules — recording, day/night switching, IR
illumination, PTZ guard tours, MQTT publishing — to events like *Sunset*,
*Full moon*, or *June solstice* through the camera's standard Action Rules
picker.

AXIS OS has no native sunrise/sunset trigger: the DayNight API reacts to ambient
lux, and Event Schedules support only fixed time-of-day rules that can't
recompute against location and date. This app fills that gap entirely on-device,
with no cloud dependency and no external services.

The app runs MIT-licensed and open source — source, CI, and release artifacts
are all public.

## Download

Pick the `.eap` matching your camera's SoC architecture from the
[latest release](https://github.com/drcoble/Camera_Schedule/releases):

| Architecture | Targets |
|---|---|
| armv7hf | Artpec-7 (and earlier) on AXIS OS 11.11+ or OS 12.x |
| aarch64 | Artpec-8+ on OS 12.x |

Install through the camera's **Apps** UI (upload the `.eap`, then Start), or via
VAPIX from the command line. Verify downloads against the release's
`SHA-256SUMS.txt`, or rebuild from source and confirm a byte-identical hash (the
build is reproducible — see [`docs/release-pipeline.md`](./docs/release-pipeline.md)).

## What it computes

All times are computed on-device from the camera's stored latitude/longitude and
IANA timezone, using Meeus astronomical algorithms (no network calls, no lookup
tables). The app declares **22 built-in event topics**:

- **Solar (10)** — sunrise, sunset, solar noon, solar midnight, and the civil /
  nautical / astronomical dawn-and-dusk twilight pairs. Meeus solar-position
  model.
- **Lunar (8)** — moonrise, moonset, lunar transit, lunar anti-transit (daily,
  parallax-aware via hourly altitude sampling), plus the four phase events: new
  moon, first quarter, full moon, last quarter.
- **Seasonal (4)** — March equinox, June solstice, September equinox, December
  solstice. Hemisphere-aware labels (e.g. *Longest Day* / *Shortest Day*) render
  in the Action Rules UI based on the camera's latitude.

On top of the built-ins, operators define their own topics at runtime:

- **Schedule anchors** — a named event built from a base event plus an optional
  offset and a recurrence/duration policy. Four variants: pulse, stateful-offset,
  paired-interval, and numeric-threshold (e.g. "fire when the moon is more than
  N% illuminated").
- **Calendar entries** — user-defined single dates, date ranges, and annually
  recurring dates.

At polar latitudes where the sun or moon never rises/sets on a given day, the
rise/set topics simply don't fire for that day while noon / midnight / transit
topics stay armed.

## Architecture

The application is a single ACAP binary that boots a GLib main loop, serves a
web UI and a JSON API over FastCGI, and arms GLib timer sources that fire camera
event topics at the right local times.

```
                Camera geolocation + clock (VAPIX / D-Bus)
                                  |
                                  v
   astro/ ──► solar.c · lunar.c · seasonal.c   (Meeus algorithms)
                                  |
                                  v
   timers.c  ── arms GLib timer sources ──►  AXEvent topics
      ▲                                          │ fire
      │ recompute                                ▼
   anchors.c · calendar.c                  camera Action Rules
      │  (operator-defined topics)         (recording, MQTT, PTZ, …)
      │
   persistence.c ──► localdata/*.json  (atomic write + schema validate)

   main.c  ── GLib main loop · FastCGI endpoints · AXParameters · UI
   status.c ── 50-entry recompute ring buffer ──► GET /state
```

### Source map (`app/src/`)

| File | Responsibility |
|---|---|
| `main.c` | Boots the GLib loop, registers FastCGI endpoints and AXParameters, serves the UI, rebinds hemisphere-aware seasonal labels on boot and location change. |
| `timers.c` | The scheduler core. Owns the four timer-slot patterns and `timers_recompute_now()`. |
| `astro/solar.c` · `lunar.c` · `seasonal.c` | Self-contained astronomical computations (Meeus). Built and unit-tested on the host. |
| `anchors.c` | Operator-defined schedule anchors: CRUD, persistence, and timer-slot construction. |
| `calendar.c` | User calendar entries: single date, date range, annual recurrence. |
| `persistence.c` | Atomic JSON writer — write-temp → fsync → parse-back → schema-validate → rename. |
| `status.c` | In-memory recompute ring buffer (50 entries, mutex-protected). |
| `log.h` | Shared logging macros, including the runtime-gated `LOG_DBG`. |

Vendored under `app/src/acap/`: the MIT-licensed `ACAP.c/h` event/HTTP framework
and `cJSON.c/h`.

### The four scheduler patterns

`timers.c` coexists four kinds of timer slots, distinguished by how often they
re-arm and what triggers a rebuild:

| Pattern | Events | Cadence | Re-arm |
|---|---|---|---|
| **Daily slots** | 10 solar + 4 daily lunar | per UTC date | recomputed at local civil midnight |
| **Phase slots** | 4 lunar phases | ~29.5 d | re-arm in own fire callback; armed once at boot |
| **Season slots** | 4 seasonal | ~91 d | re-arm in own fire callback; armed once at boot |
| **Anchor slots** | operator anchors | dynamic | rebuilt per recompute from anchor definitions |

Daily slots are recomputed when the date rolls over, on configuration change,
and on location/timezone change. Phase, season, and numeric-threshold-day slots
are observer-independent and aren't disturbed by recomputes that don't move the
satisfying-day set.

Each topic can be individually enabled or disabled. Disabling suppresses firing
only — the topic stays declared on the event engine so existing action-rule
bindings don't break. The gate is a single check at the top of every slot-arm
helper.

### Web UI and JSON API

The settings page (`schedule.html`) plus four more HTML pages
(`location`, `anchors`, `calendar`, `about`) are served from the camera. All
assets are bundled locally — no external CDN. The pages talk to **eleven FastCGI
endpoints**:

| Endpoint | Access | Purpose |
|---|---|---|
| `about` | viewer | App name, version, architecture. |
| `location` | admin | Read / override the camera's lat/lon and timezone. |
| `anchors` | admin | CRUD for operator schedule anchors. |
| `calendar` | admin | CRUD for calendar entries. |
| `events` | admin | List declared topics and their enable state. |
| `events_today` | viewer | Today's computed firing times. |
| `state` | viewer | Live status snapshot + recompute ring buffer + RSS. |
| `recompute` | admin | Force a recompute/re-arm now. |
| `export` | admin | Download full config as a versioned JSON envelope. |
| `import` | admin | Upload + schema-validate a config envelope. |
| `debug` | admin | Toggle runtime debug logging. |

Four scalar settings are also exposed as **AXParameters** under
`root.Camera_schedule.*` (`LookaheadDays`, `EventNamePrefix`,
`PollIntervalSeconds`, `DebugLogging`), so they're configurable through standard
camera tooling.

### State and persistence

Operator configuration persists as JSON under `localdata/`:
`anchors.json`, `calendar.json`, and `schedule_enabled.json` (the enable map).
Every write goes through the atomic helper in `persistence.c`. Config
export/import uses a versioned `camera-schedule.config.v1` envelope with schema
validation on import.

The camera's geolocation service remains the source of truth for lat/lon; manual
overrides write back to it via `/axis-cgi/geolocation/set.cgi` rather than
keeping a parallel store.

## Building from source

The build is fully Dockerized — the only host requirement is Docker (Buildx) and
`make`. The toolchain, SDK, manifest validator, and `acap-build` packaging all
run inside the pinned `axisecp/acap-native-sdk` image.

```sh
make -C app build-armv7hf   # OS 11.11+ and OS 12 on Artpec-7
make -C app build-aarch64   # OS 12 on Artpec-8+
make -C app build-all       # both
make -C app help            # menu
```

Outputs land in `dist/camera-schedule-<arch>.eap`. The astronomical modules and
config round-trip are unit-tested on the host under `app/test/host/`. See
[`CONTRIBUTING.md`](./CONTRIBUTING.md) for the full build/test/install loop and
DCO sign-off policy.

## Compatibility

- **AXIS OS 11.11+ and OS 12.x** from a single mainline.
- Two artifacts: **armv7hf** (Artpec-7 and earlier) and **aarch64** (Artpec-8+).
- Conforms to ACAP v12 sandboxing, dynamic-user, and D-Bus credential
  requirements.

## Project docs

- [`CHANGELOG.md`](./CHANGELOG.md) — per-release history.
- [`CONTRIBUTING.md`](./CONTRIBUTING.md) · [`SECURITY.md`](./SECURITY.md) ·
  [`CODE_OF_CONDUCT.md`](./CODE_OF_CONDUCT.md) — contribution, vulnerability
  reporting, and conduct.
- [`docs/release-pipeline.md`](./docs/release-pipeline.md) — build determinism
  and the release machinery.
- [`IMPLEMENTATION.md`](./IMPLEMENTATION.md) — milestone roadmap.
- [`requirements/`](./requirements) — detailed functional and cross-cutting
  requirements, plus the append-only [decision log](./requirements/28-decision-log.md).

## License

MIT — see [`LICENSE`](./LICENSE). Third-party components (the vendored Timelapse2
ACAP framework and cJSON, both MIT) are credited in `THIRD_PARTY_LICENSES.md` on
the release page.
