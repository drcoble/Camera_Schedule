# FR-13 — Logging

Logs are the primary field-debugging tool on Axis cameras. The app must log
enough to diagnose schedule mismatches without flooding the journal.

## Requirements

- **FR-13.1 — Syslog backend.** The app SHALL log via the standard C
  `syslog(3)` API. It SHALL call `openlog()` with the manifest `appName` (or
  a short alias such as `ssc`) as the program identifier.

- **FR-13.2 — Levels.** Log entries SHALL use the following discipline:

  | Level | Used for |
  |---|---|
  | `LOG_ERR` | VAPIX write failures persisting past 1 h, invalid location, config load failure post-upgrade. |
  | `LOG_WARNING` | Polar "no event today" reports, individual write retries, NTP not yet synced, transient API failures. |
  | `LOG_INFO` | Recompute summaries (per [FR-10.4](./10-recompute-cadence.md)), config save, lat/lon or timezone change detected. |
  | `LOG_DEBUG` | Per-event computed times, full HTTP request/response bodies. **Off by default.** |

- **FR-13.3 — In-UI ring buffer.** The app SHALL retain the **last 50
  recompute summaries** in an in-memory ring buffer and surface them in the
  configuration UI status panel ([FR-11.2](./11-configuration-ui.md)). This
  lets a field operator inspect recent history without SSH access to the
  camera.

- **FR-13.4 — Debug toggle.** A "verbose logging" toggle SHALL be available
  in the UI. When enabled, the app SHALL emit `LOG_DEBUG` entries. The
  toggle SHALL persist across restarts but SHALL NOT be the default.

- **FR-13.5 — No PII or credentials in logs.** D-Bus-issued VAPIX
  credentials, full URLs containing credentials, and any user-entered free
  text from notes fields SHALL NOT appear in logs at any level.
