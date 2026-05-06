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

- **FR-11.6 — Schedule list view.** The UI SHALL present every schedule
  the app exposes — built-in solar / lunar / seasonal events,
  operator-defined anchors ([FR-7](./07-schedule-anchors.md)), and
  calendar entries ([FR-6](./06-user-calendar-dates.md)) — in a unified
  **Schedule list view** with the following structure:

  - **Grouping.** Schedules SHALL be organized into labeled, collapsible
    sections (Solar events, Lunar events, Seasonal events, User
    anchors, Calendar entries). Each section header SHALL display the
    count of enabled vs. total schedules in that section.
  - **Search.** A single text-input search field SHALL filter visible
    rows by name substring in real time (no submit action). Sections
    that contain no matching rows SHALL collapse automatically while a
    filter is active.
  - **Per-row content.** Each schedule row SHALL display, at minimum:
    an enable/disable toggle, the schedule's human name, and the
    computed next-fire time for the current day — or a diagnostic
    phrase such as `not firing today` (polar / no-event-today,
    [FR-3](./03-solar-events.md)) or `disabled — topic registered`
    (toggled off, [FR-11.7](#fr-117-schedule-enabledisable-semantics)).
  - **Inline actions.** Operator-defined anchor and calendar-entry
    rows SHALL additionally present **Edit** and **Delete** inline
    actions. Built-in solar / lunar / seasonal event rows SHALL NOT
    present a Delete action; they SHALL show a non-deletable visual
    indicator (e.g. a lock glyph).
  - **Save semantics.** Toggle-state changes SHALL be persisted
    immediately on click without a separate page-level Save action.
    The row SHALL display a transient `Saving…` indicator and SHALL
    revert the toggle and display a `Failed — retry?` message on a
    backend error. Writes SHALL target the `events` FastCGI endpoint
    (admin-gated per [DL-13](./28-decision-log.md)).

  The list view MUST scale gracefully past 50 rows (M3 reaches ~22
  built-ins, M6 adds up to 64 user-defined anchors plus calendar
  entries) without pagination; section grouping plus the search field
  is the entire navigation strategy.

- **FR-11.7 — Schedule enable/disable semantics.** The app SHALL
  maintain a persistent enable-state store at
  `localdata/schedule_enabled.json`, keyed by schedule ID, with the
  following contract:

  - **Default-enabled.** Absent keys SHALL be treated as enabled. A
    newly added built-in (introduced by an `.eap` upgrade) or
    operator-defined schedule SHALL fire on its next computed time
    until the operator explicitly disables it.
  - **Suppress firing, preserve declaration.** Disabling a schedule
    SHALL prevent the recompute pipeline from arming a GLib timer for
    that schedule (see [FR-9.2](./09-event-firing.md) and
    [FR-8.8](./08-event-registration.md)). Disabling SHALL NOT
    undeclare the AXEvent topic. The topic SHALL remain visible in
    the camera's Action Rules UI and SHALL accept new operator
    bindings while disabled. Re-enabling SHALL resume firing on the
    next recompute cycle without changing the topic's declared form.
  - **Orphan keys.** Enable-state entries for IDs that no longer
    correspond to a registered schedule (e.g. a deleted user anchor)
    SHALL be silently ignored at boot. Active cleanup is not required.
  - **Survival.** The store SHALL survive reboot and `.eap` upgrade
    via the existing [FR-12](./12-configuration-persistence.md)
    `localdata/` persistence model.

  This separation is captured in [DL-18](./28-decision-log.md): the
  registration path (`ax_event_handler_declare`) is unaffected by
  enable/disable; only the firing path
  (`arm_event_slot` in `timers.c`) is gated.

## Notes

- The reverse-proxy + FastCGI path is used uniformly across both
  supported OS lines (AXIS OS 11.11+ and 12.x), matching the
  Timelapse2 precedent.
