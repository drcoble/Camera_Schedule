# FR-12 — Configuration persistence

Configuration must survive restarts and firmware upgrades, must not wear out
flash with frequent writes, and must be portable between cameras.

## Requirements

- **FR-12.1 — Primary store.** Anchor and calendar-entry configuration SHALL
  persist as a **single JSON file** in the app's `localdata/` directory:
  - written **atomically** (write-temp + `fsync` + `rename`),
  - validated against a schema before the rename step,
  - rolled back on validation failure.

- **FR-12.2 — AXParameter exposure.** Simple scalar settings — recompute
  look-ahead window, schedule-name prefix, poll interval — SHALL also be
  exposed via the **AXParameter system** so they appear in the standard
  camera parameter UI and are accessible via VAPIX `param.cgi` for fleet
  tooling. Lat/lon is **not** persisted by this app — it lives in the
  camera's geolocation service per [FR-1.3](./01-geo-location.md).

- **FR-12.3 — Export / import.** The configuration UI SHALL offer:
  - **Export**: download the current JSON config as a file.
  - **Import**: upload a JSON config file. Imports SHALL be schema-validated
    and SHALL fully replace the current config on success. The import SHALL
    trigger an immediate recompute.

- **FR-12.4 — Upgrade behavior.** On firmware or app upgrade:
  - The `localdata/` JSON file SHALL be preserved.
  - On startup, the app SHALL apply forward-compatible schema migrations
    where needed and SHALL log the migration at INFO.
  - If the file is unreadable or fails validation post-upgrade, the app
    SHALL preserve the original file (renamed `.broken-<timestamp>`), start
    with a default config, and log at ERR. The UI SHALL surface the failure.

- **FR-12.5 — Flash wear.** State writes SHALL be coalesced. The JSON file
  SHALL NOT be rewritten more than **once per minute** except on explicit
  user save. Per-recompute status (last run time, error counters) is held
  in memory and surfaced via the UI ring buffer
  ([FR-13.3](./13-logging.md)) — not persisted on every tick.

## Notes

- Single-file JSON keeps the on-disk format trivial to inspect, diff, and
  hand-edit when debugging in the field.
- AXParameter duplication (FR-12.2) exists so existing fleet-management
  tooling that already drives `param.cgi` can configure simple knobs without
  needing to learn the app's REST surface.
