# Reuse from Timelapse2

Fred Juhlin's open-source **Timelapse2** ACAP
(<https://github.com/pandosme/Timelapse2>, MIT) demonstrates the
publish-ACAP-event-topics pattern (Path A in
[OQ-10](./24-open-questions.md)) and ships a vendored mini-framework that
covers most of the plumbing Camera_Schedule needs. Camera_Schedule is also
MIT, so direct in-tree code reuse is permitted with attribution.

This file enumerates what we lift, what we extend, and what we replace.

## Direct lift (vendor with original headers)

These files SHALL be vendored from Timelapse2 with their original MIT
copyright lines preserved (Fred Juhlin, 2024). Add SPDX `MIT` headers if
not already present and record them in `THIRD_PARTY_LICENSES.md`
([DR-10](./25-licensing-and-distribution.md)).

| Source (Timelapse2) | Target (Camera_Schedule) | Role |
|---|---|---|
| `app/ACAP.c` | `app/src/acap/ACAP.c` | mini-framework: HTTP/FastCGI, AXEvent wrappers, AXParameter, VAPIX GET/POST, `ACAP_DEVICE_*` |
| `app/ACAP.h` | `app/src/acap/ACAP.h` | public surface of the framework |
| `app/cJSON.c` | `app/src/acap/cJSON.c` | JSON parser used everywhere |
| `app/cJSON.h` | `app/src/acap/cJSON.h` | |

These give us out-of-the-box:

- `ax_event_handler_*` wrappers — declare topics, fire pulse and stateful
  events with the topic shape
  `tnsaxis:CameraApplicationPlatform/<appName>/<eventId>`
  (the convention operators see in the camera's Action Rules UI),
- VAPIX loopback helpers (`ACAP_VAPIX_Get/Post`), which internally call
  the D-Bus `VAPIXServiceAccounts1.GetCredentials` method (used by the
  geolocation read/write path in [FR-1](./01-geo-location.md), not by
  the event firing path),
- `ACAP_DEVICE_Latitude / _Longitude / _Set_Location` calling
  `/axis-cgi/geolocation/{get,set}.cgi` — covers our
  [FR-1](./01-geo-location.md) needs end-to-end,
- FastCGI endpoint registration via `ACAP_HTTP_Node(name, handler)`
  matched to manifest `httpConfig` entries,
- a status JSON store surfaced over HTTP — slots straight into the
  [FR-11.2](./11-configuration-ui.md) status panel,
- a config persistence layer that survives restart and upgrade.

## Pattern adoption (write our own, follow Timelapse2's shape)

These are conventions, not code. We replicate the structure but write our
own files because they're project-specific.

- **Declarative event list** in `app/settings/events.json`, one entry per
  schedulable event the app exposes:
  ```json
  [
    {"id":"sunrise","name":"Sunrise","state":false,"show":true},
    {"id":"sunset","name":"Sunset","state":false,"show":true},
    {"id":"sunnoon","name":"Solar noon","state":false,"show":true},
    {"id":"sunmidnight","name":"Solar midnight","state":false,"show":true},
    {"id":"civildawn","name":"Civil dawn","state":false,"show":true},
    {"id":"civildusk","name":"Civil dusk","state":false,"show":true},
    {"id":"nauticaldawn","name":"Nautical dawn","state":false,"show":true},
    {"id":"nauticaldusk","name":"Nautical dusk","state":false,"show":true},
    {"id":"astrodawn","name":"Astronomical dawn","state":false,"show":true},
    {"id":"astrodusk","name":"Astronomical dusk","state":false,"show":true},
    {"id":"moonrise","name":"Moonrise","state":false,"show":true},
    {"id":"moonset","name":"Moonset","state":false,"show":true},
    {"id":"moonnoon","name":"Lunar transit","state":false,"show":true},
    {"id":"moonmidnight","name":"Lunar anti-transit","state":false,"show":true},
    {"id":"newmoon","name":"New moon","state":false,"show":true},
    {"id":"firstquarter","name":"First quarter moon","state":false,"show":true},
    {"id":"fullmoon","name":"Full moon","state":false,"show":true},
    {"id":"lastquarter","name":"Last quarter moon","state":false,"show":true},
    {"id":"summersolstice","name":"Summer solstice","state":false,"show":true},
    {"id":"wintersolstice","name":"Winter solstice","state":false,"show":true},
    {"id":"marchequinox","name":"March equinox","state":false,"show":true},
    {"id":"septequinox","name":"September equinox","state":false,"show":true}
  ]
  ```
  At init, the framework iterates the list and calls
  `ACAP_EVENTS_Add_Event(...)` for each, which internally calls
  `ax_event_handler_declare(...)`.
- **User-defined calendar entries** ([FR-6](./06-user-calendar-dates.md))
  generate dynamic event topics like
  `tnsaxis:CameraApplicationPlatform/<appName>/calendar_<id>` — registered
  at the same boot stage from the persisted config. Operators see them in
  the Action Rules UI.
- **Anchor-with-offset events** ([FR-7](./07-schedule-anchors.md)) are
  *also* registered as their own event topics so that an anchor "30 min
  before sunrise" surfaces as a distinct topic an action rule can bind to,
  rather than requiring action rules to know about offsets. This is a
  small departure from Timelapse2 (which has no anchor concept), and it
  keeps action-rule wiring simple.
- **Daily recompute via GLib timers** (Timelapse2's pattern in
  `app/sunevents.c`):
  - one **midnight timer source** that fires once a day, recomputes the
    full event list for the new day, and arms today's per-event timers,
  - one one-shot **per-event timer** for each event-time scheduled today,
    each firing the corresponding ACAP event topic via
    `ACAP_EVENTS_Fire(eventId)`.
  - Same mechanism handles geolocation/timezone change (cancel + rearm).
- **FastCGI backend endpoints** for the web UI (Timelapse2 uses `app`,
  `settings`, `status`, `sunevents`, `reset`). Camera_Schedule analogues:

  | Endpoint | Method | Purpose |
  |---|---|---|
  | `app` | GET | static metadata (version, name) |
  | `status` | GET | last/next recompute, lat/lon, TZ, errors |
  | `location` | GET/POST | read or override camera lat/lon |
  | `anchors` | GET/POST | anchor CRUD ([FR-7](./07-schedule-anchors.md)) |
  | `calendar` | GET/POST | calendar-entry CRUD ([FR-6](./06-user-calendar-dates.md)) |
  | `events_today` | GET | preview computed event times for today/N-days |
  | `recompute` | POST | manual trigger ([FR-10.2](./10-recompute-cadence.md)) |
- **Static UI in `app/html/`** served by the camera's web server at
  `/local/<appName>/`. Bundle Bootstrap and jQuery locally as Timelapse2
  does — but **NOT** Leaflet from a CDN; see "Replace" below.

## Extend / write our own

Where Timelapse2 falls short, Camera_Schedule has to do new work.

- **Solar math.** Timelapse2's `sunevents.c` computes only sunrise, sunset,
  solar noon, and civil dawn/dusk — using a sound NOAA-style algorithm
  but no polar safety (`acos` clamping), no nautical/astronomical twilight,
  and no solar midnight. Camera_Schedule's
  [FR-3](./03-solar-events.md) requires all twilight altitudes plus polar
  `NO_EVENT_TODAY` sentinels. We refactor Timelapse2's solar core into
  `app/src/astro/solar.c` parameterized by zenith-angle, then add the
  three twilight zeniths and proper edge-case handling.
- **Lunar math.** Timelapse2 has none. Camera_Schedule adds
  `app/src/astro/lunar.c` per [FR-4](./04-lunar-events.md): Meeus ch. 47
  position, ch. 49 phases, parallax-aware moonrise/moonset via hourly
  altitude interpolation.
- **Seasonal math.** New `app/src/astro/seasonal.c`
  ([FR-5](./05-seasonal-events.md)) — solstices/equinoxes via Meeus ch. 27.
- **Schedule anchors and offsets.** Timelapse2 has no concept of
  user-defined anchors with signed offsets and durations. New module
  `app/src/anchors.c` driven by the persisted config from `localdata/`.
- **User-defined calendar entries.** Timelapse2 has none. New module
  `app/src/calendar.c`.
- **Hemisphere-aware "longest/shortest day" labels** ([FR-5.2](./05-seasonal-events.md)).

## Replace

Things in Timelapse2 we deliberately **do not** copy.

- **Leaflet from `unpkg.com` and OSM tile servers.** Violates our
  [FR-11.3](./11-configuration-ui.md) "no external CDN" rule and breaks
  on air-gapped camera networks. Two acceptable options:
  1. Bundle Leaflet's JS/CSS locally in `app/html/vendor/leaflet/` (no
     map tiles, just a coordinate widget; user clicks an empty canvas
     with a graticule overlay). MIT-licensed, ~150 KB.
  2. Skip the map entirely in v1; provide a numeric lat/lon form and a
     "use camera's GPS-set location" button. Smaller `.eap`, simpler.

  Recommendation: option 2 for v1; revisit if users ask for a map.
- **Storage group permission** (`resources.linux.user.groups: ["storage"]`).
  Timelapse2 needs SD-card writes for video; we don't write video, only
  text config — drop this from our manifest.
- *(no items remaining — single-mainline OS 11.11+ floor adopted per
  [DL-15](./28-decision-log.md))*

## Attribution requirements

- `THIRD_PARTY_LICENSES.md` SHALL include the verbatim MIT header from
  Timelapse2's `app/LICENSE` and identify which files were lifted.
- Lifted files SHALL keep their original copyright lines. Modifications
  SHALL be marked with a `// Modified for Camera_Schedule, 2026:` comment
  block at the change site.
- The release `NOTICE`-style attribution surface is `THIRD_PARTY_LICENSES.md`
  per [DR-3](./25-licensing-and-distribution.md); MIT does not require a
  separate NOTICE file.

## Sources

- Timelapse2 repo: <https://github.com/pandosme/Timelapse2>
- License: `app/LICENSE` (MIT, Fred Juhlin 2024)
- Reusable framework references: `app/ACAP.c` lines 1005, 1126, 1611,
  1618, 1946, 2008
- Solar reference: `app/sunevents.c` `Calculate_Sun_Events`,
  `Setup_Midnight_Timer`, `Setup_SunNoon_Timer`
- Sibling scaffolding repo: <https://github.com/pandosme/make_acap>
  (template; license-status mixed — review before lifting)
