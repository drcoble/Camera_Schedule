# FR-10 — Recompute cadence

When the app recomputes, what triggers a recompute, and how the next-run is
chosen.

## Requirements

- **FR-10.1** The app SHALL recompute schedules:
  - on app start,
  - after any configuration change (anchor edits, calendar-entry edits,
    lat/lon override, prefix change, look-ahead window change),
  - after a detected change to the camera's lat/lon ([FR-1.5](./01-geo-location.md)),
  - after a detected change to the camera's timezone ([FR-2.3](./02-time-and-timezone.md)),
  - **daily**, at local solar midnight ([FR-3.3](./03-solar-events.md)) when
    available; otherwise at **02:30 local time** as a fallback (e.g. polar
    regions or when location is invalid).
- **FR-10.2** A **"Recompute now"** button SHALL be available in the
  configuration UI for manual triggering.
- **FR-10.3** Recompute runs SHALL be serialized — at most one in flight. A
  trigger that arrives during an in-flight recompute SHALL coalesce into a
  single follow-up run.
- **FR-10.4** Each recompute SHALL emit an INFO-level syslog summary
  including: trigger reason, lat/lon used, timezone, count of anchors
  evaluated, count of `VEVENT`s emitted, count of `POST/PATCH/DELETE`
  operations applied, and total elapsed time.

## Notes

- Solar midnight as the daily anchor avoids straddling the boundary of any
  generated `VEVENT` — solar midnight is the deepest astronomical "off"
  point. Falling back to 02:30 local handles the polar / invalid-location
  cases where solar midnight is undefined.
- Coalescing (FR-10.3) keeps a burst of UI edits from stampeding the camera
  with redundant write batches.
