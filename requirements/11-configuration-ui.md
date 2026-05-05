# FR-11 — Configuration UI

The app exposes a configuration page in the camera's web interface. This is
the only operator-facing surface; everything else (schedules, action rules)
is wired up in the camera's own UI.

## Requirements

- **FR-11.1 — Surface.** The app SHALL expose its configuration page in the
  camera's web UI via the **reverse-proxy / FastCGI** mechanism documented
  by Axis. It SHALL be integrated with the camera's session auth model:
  - **admin**: read + write,
  - **operator** / **viewer**: read-only.

- **FR-11.2 — Capabilities.** The UI SHALL allow the operator to:
  - View the detected lat/lon and timezone, with an indicator of whether the
    Geolocation API reports `ValidPosition=true`.
  - Set / clear a manual lat/lon override (FR-1.3).
  - Define, edit, reorder, and delete **calendar entries** (FR-6).
  - Define, edit, enable/disable, reorder, and delete **schedule anchors**
    (FR-7).
  - **Preview** the next *N* occurrences of any anchor (default 14) for
    verification before saving.
  - View a **status panel** showing: last recompute time, last recompute
    trigger reason, next scheduled recompute time, current lat/lon and
    timezone in use, count of managed schedules on the camera, and any
    pending or persistent write errors.
  - Trigger a manual **"Recompute now"** (FR-10.2).
  - Export and import configuration (see [FR-12.3](./12-configuration-persistence.md)).

- **FR-11.3 — No external dependencies.** The UI SHALL be vanilla
  HTML / CSS / JS bundled with the .eap. **No external CDN, font, map
  tile, or asset loads** — cameras are frequently deployed on isolated
  networks. All assets (Bootstrap, jQuery if used, any map library)
  SHALL be served from the app's own `/local/<appName>/` static path.
  This is an explicit departure from Timelapse2, whose Location tab
  pulls Leaflet from `unpkg.com` and OSM tiles from the public internet
  (see [27-reuse-from-timelapse2.md](./27-reuse-from-timelapse2.md)). For
  Camera_Schedule v1, the location UI SHALL be a numeric lat/lon form
  plus a "use camera's GPS-set location" button — no map.

- **FR-11.4 — Accessibility & localization.**
  - All controls SHALL be keyboard-reachable.
  - Source text SHALL be English. A future locale layer is out of scope but
    the UI structure SHALL keep strings in a single resource bundle to
    leave that door open.

- **FR-11.5 — Validation.** Form fields SHALL validate client-side and again
  server-side. Invalid input (out-of-range lat/lon, malformed dates,
  duplicate anchor names) SHALL be rejected with a clear error message and
  SHALL NOT persist.

## Notes

- The reverse-proxy + FastCGI path is used uniformly across both
  supported OS lines (AXIS OS 11.11+ and 12.x), matching the
  Timelapse2 precedent.
