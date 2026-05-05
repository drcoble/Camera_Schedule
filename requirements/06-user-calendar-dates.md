# FR-6 — User-defined calendar dates

The "calendar" surface is operator-curated. The app does **not** ship a
holiday or locale database; the operator enters whatever dates matter for
their site.

## Requirements

- **FR-6.1** The operator SHALL be able to define named calendar entries of
  three kinds:
  - **Single date** — one specific date.
  - **Date range** — inclusive `start_date` / `end_date`.
  - **Annual fixed date** — recurring every year on the same month/day
    (e.g. every Dec 24).
- **FR-6.2** Each entry SHALL carry:
  - a human name (required, used in iCalendar `SUMMARY`),
  - either a specific time-of-day **or** an "all-day" flag,
  - optional notes (free-text, not used in scheduling).
- **FR-6.3** No bundled holiday database is in scope. Easter, Ramadan, Chinese
  New Year, and other computed holidays are out of scope; the operator may
  enter them manually.
- **FR-6.4** Calendar entries SHALL be exposable as schedule anchors (see
  [FR-7](./07-schedule-anchors.md)) in exactly the same way as solar/lunar
  events. An anchor referencing a calendar entry MAY apply an offset and a
  duration.

## Notes

- Keeping calendar input user-defined avoids locale-specific bugs, license
  questions around bundled holiday databases, and the maintenance burden of
  shipping holiday-table updates with the firmware.
- Operators who need locale holiday tables can paste them in from external
  sources or use a separate fleet-management tool.
