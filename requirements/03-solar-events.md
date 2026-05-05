# FR-3 — Solar event computation

The app computes solar events per local civil day for the camera's lat/lon. All
solar events are exposable as schedule anchors (see [FR-7](./07-schedule-anchors.md)).

## Requirements

The app SHALL compute, per local civil day, the following solar events:

- **FR-3.1 — Sunrise / sunset.** Geometric solar disk center crossing zenith
  **90.833°**, including standard atmospheric refraction (~34′) and the solar
  semi-diameter (~16′).
- **FR-3.2 — Solar noon** ("sun highest in the sky" — solar transit / upper
  culmination).
- **FR-3.3 — Solar midnight** ("sun lowest in the sky" — solar anti-transit /
  lower culmination). Always exists, even in polar regions.
- **FR-3.4 — Civil dawn / civil dusk.** Sun center at zenith **96°**.
- **FR-3.5 — Nautical dawn / nautical dusk.** Sun center at zenith **102°**.
- **FR-3.6 — Astronomical dawn / astronomical dusk.** Sun center at zenith **108°**.

## Accuracy

- **FR-3.7** Per-event accuracy SHALL be ≤ **60 s** vs. USNO reference data at
  latitudes ≤ ±60°, and ≤ **5 min** at latitudes ≤ ±66.5°.

## Polar handling

- **FR-3.8** Above latitudes of ±66.5° (Arctic / Antarctic), where a solar
  event may not occur on a given day:
  - the algorithm SHALL return a `NO_EVENT_TODAY` sentinel,
  - the app SHALL **omit** such events from generated iCalendar payloads
    (no garbage times),
  - the omission SHALL be logged at INFO level,
  - solar noon / solar midnight (FR-3.2 / FR-3.3) remain valid year-round and
    SHALL still be emitted.

## Implementation notes

- Use a vendored, in-tree, MIT/BSD/public-domain C translation of the
  NOAA/Meeus solar algorithm. Do not link libnova (LGPL) or NREL SPA
  (noncommercial) — see [build & packaging](./22-build-and-packaging.md).
- A single position-of-the-sun routine drives all of FR-3.1 – FR-3.6 by
  varying the target zenith angle.
