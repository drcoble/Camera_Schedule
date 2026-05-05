# FR-5 — Seasonal / astronomical-calendar events

Annual astronomical events the app exposes as schedule anchors.

## Requirements

- **FR-5.1** The app SHALL compute, per Gregorian year, the dates and UTC
  instants of:
  - **June solstice**,
  - **December solstice**,
  - **March equinox**,
  - **September equinox**.
- **FR-5.2 — Hemisphere-aware labels.** User-facing labels SHALL be derived
  from the sign of the camera's latitude:
  - In the northern hemisphere, the June solstice is labeled **"Longest Day"**
    and the December solstice **"Shortest Day"**.
  - In the southern hemisphere, the labels are inverted.
  - On the equator (|lat| < 0.5°), neutral labels ("June solstice",
    "December solstice") SHALL be used.
- **FR-5.3** Equinox labels SHALL be presented as "March equinox" and
  "September equinox" (hemisphere-neutral) by default; the UI MAY offer a
  hemisphere-aware "Spring / Autumn equinox" rendering as a presentation
  option but the canonical name in stored config SHALL remain the
  hemisphere-neutral form.

## Implementation notes

- Closed-form approximations from Meeus *Astronomical Algorithms* ch. 27 are
  sufficient (≤ 1 minute accuracy across the modern era) and avoid pulling
  in heavy ephemeris machinery.
- Solstice and equinox events are point-in-time anchors (pulse, not
  interval).
