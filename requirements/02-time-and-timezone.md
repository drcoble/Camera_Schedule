# FR-2 — Time and timezone

All computations are performed in UTC and rendered to the camera's local
timezone for emission. DST transitions must not produce duplicate or missed
schedule events.

## Requirements

- **FR-2.1** The app SHALL read the camera timezone via the Time API
  (`POST /axis-cgi/time.cgi` method `getDateTimeInfo`) and prefer the IANA name
  (e.g. `Europe/Stockholm`) over the POSIX form.
- **FR-2.2** Internal celestial computation SHALL be performed in UTC. Output
  iCalendar events SHALL carry an explicit `TZID` parameter naming the IANA
  zone, with `DTSTART` / `DTEND` in local civil time.
- **FR-2.3** The app SHALL detect timezone changes on its poll interval (FR-1.1)
  and trigger an immediate recompute.
- **FR-2.4** The app SHALL rely on the camera's NTP-synced clock. It SHALL NOT
  embed its own NTP client or maintain its own time source.
- **FR-2.5** DST transitions SHALL be handled correctly. Computed events SHALL
  remain accurate across spring-forward and fall-back boundaries — no duplicate
  triggers in the repeated hour, no missed triggers in the skipped hour.
- **FR-2.6** If the camera reports its time as not yet synced (NTP not
  converged), the app SHALL defer the first recompute and log a WARN until the
  clock is valid. Last-known-good schedules SHALL remain in place.

## Notes

- Whether AXIS OS ships an up-to-date zoneinfo database accessible from inside
  the ACAP sandbox is unconfirmed in public docs — see
  [open questions](./24-open-questions.md). If unavailable, a tzdata snapshot
  must be vendored.
