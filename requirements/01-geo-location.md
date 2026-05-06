# FR-1 — Geographic location

The camera's lat/lon is the primary input to every celestial computation. Wrong
or unset coordinates produce wrong schedules, so reading and validating them
must be explicit and observable.

## Requirements

- **FR-1.1** The app SHALL read latitude and longitude from the camera's
  Geolocation API (`GET /axis-cgi/geolocation/get.cgi`) on startup and on a
  configurable poll interval (default **1 hour**).
- **FR-1.2** If the API response field `ValidPosition` is `false`, or the API
  is unavailable, the app:
  - SHALL surface a clear error in the configuration UI (FR-11),
  - SHALL NOT publish stale or fabricated schedules,
  - SHALL retain the last successful schedule set already on the camera,
  - SHALL emit a WARN-level syslog entry on each failed recompute attempt.
- **FR-1.3** The app SHALL allow the operator to enter lat/lon via its
  configuration UI when the camera's stored location is unset, invalid,
  or known-wrong. Following the Timelapse2 precedent
  ([27-reuse-from-timelapse2.md](./27-reuse-from-timelapse2.md)), the UI
  override SHALL be **written back to the camera's geolocation service**
  via `/axis-cgi/geolocation/set.cgi`, making the camera's geolocation
  the single source of truth for lat/lon. The app SHALL NOT maintain a
  separate persistent override in `localdata/` or AXParameter. (Side
  effect: if other apps on the camera read geolocation, they see the same
  value.)
- **FR-1.4** WGS-84 decimal degrees SHALL be the canonical internal
  representation. The DD / DMM / DMS string variants returned by the camera
  SHALL be normalized to decimal degrees on read.
- **FR-1.5** A change in detected lat/lon (vs. the value used for the last
  successful recompute) SHALL trigger an immediate recompute (see FR-10).
- **FR-1.6** The configuration UI (FR-11) SHALL accept and display lat/lon
  values to **microdegree precision (6 decimal places, ~11 cm at the
  equator)**. On read-back from the camera's geolocation service, values
  with more decimals SHALL be **rounded to the nearest microdegree**
  before being shown in the form. The HTML5 numeric-input `step` attribute
  SHALL be `any` so the form does not reject the camera's stored value
  for failing a step constraint when the camera reports finer precision
  than the UI's display step. Internal computation continues to use IEEE-754
  `double`; rounding is a UI-only behavior. See [DL-17](./28-decision-log.md).

## Notes

- The Geolocation API is the only documented on-camera location surface. GPS
  modules on certain PTZ models report through this same API, so no separate
  GPS path is needed.
- The default 1-hour poll is appropriate for fixed-mount cameras. Mobile/PTZ
  deployments may need a shorter interval (see [open questions](./24-open-questions.md)).
