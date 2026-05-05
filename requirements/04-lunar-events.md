# FR-4 — Lunar event computation

Lunar events are exposable as schedule anchors. Moonrise/moonset are notably
harder than sunrise/sunset because the Moon moves ~13°/day, parallax is
significant, and on many days a rise or a set does not occur.

## Requirements

- **FR-4.1 — Moonrise / moonset.** The app SHALL compute moonrise and moonset
  for the camera's lat/lon, accounting for **lunar parallax** (topocentric
  altitude) and using **hourly altitude sampling with linear interpolation**
  to find the horizon crossing. Days on which no rise or no set occurs SHALL
  be reported as `NO_EVENT_TODAY` and omitted from output.
- **FR-4.2 — Lunar transit / anti-transit.** The app SHALL compute lunar
  transit ("moon highest in the sky") and lunar anti-transit ("moon lowest
  in the sky").
- **FR-4.3 — Principal phases.** The app SHALL compute the four principal
  lunar phases — new moon, first quarter, full moon, last quarter — as
  point-in-time UTC instants (Meeus *Astronomical Algorithms* ch. 49).
- **FR-4.4 — Illumination fraction.** The app SHALL compute the Moon's
  illuminated fraction (0.0–1.0) at any requested instant. This is exposable
  as a numeric anchor (e.g. "trigger when illumination ≥ 0.95"); see
  [FR-7.3](./07-schedule-anchors.md#fr-73--numeric-threshold-anchors).

## Accuracy

- **FR-4.5** Lunar event accuracy SHALL be:
  - ≤ **2 min** for moonrise / moonset / transit,
  - ≤ **5 min** for principal phase instants,
  - ≤ **2 percentage points** for illumination fraction.

## Implementation notes

- Vendored, in-tree C implementation based on Meeus chapters 47 (position) and
  49 (phases). No external library dependency.
- Hourly altitude sampling spans the local civil day plus padding to catch
  rises/sets that straddle midnight.
- Topocentric altitude requires applying lunar horizontal parallax (~0.95°)
  to geocentric coordinates.
