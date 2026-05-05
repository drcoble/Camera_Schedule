# Discovered environment

Findings from read-only VAPIX probes against the lab cameras
(see [23-verification.md](./23-verification.md)). These observations
**override** any conflicting assumption in the earlier requirements that
came from public Axis documentation. Where they invalidate prior
requirements, the affected files have been amended and the relevant items
in [24-open-questions.md](./24-open-questions.md) updated.

## Camera 1 (AXIS OS 12 reference)

- **Firmware version**: `12.10.61` (build date 2026-04-15).
- **SoC**: Axis Artpec-7. **Architecture: `armv7hf`.**
- **Geolocation API present** (`Properties.Geolocation.Geolocation=yes`,
  Version `1.0`); current location set, `ValidPosition=true`.
- **Time API present** (`/axis-cgi/time.cgi`); reports IANA timezone
  (`America/New_York`), POSIX (`EST5EDT,M3.2.0,M11.1.0`), DST enabled,
  NTP synced.
- **VAPIX SOAP services reachable** at `/vapix/services` (Action Service
  namespace `http://www.axis.com/vapix/ws/action1`); `GetActionRules` works
  and returns existing rules.
- **Documented `/config/rest/scheduled-events/v2` REST API does NOT exist**
  on this firmware. v1 also returns "API not found". This finding drove
  the architecture pivot in [DL-05](./28-decision-log.md).
- **SOAP `GetSchedules`, `GetRecurrenceRules`, `GetServiceCapabilities`,
  `GetScheduledEvents`** all return SOAP fault `ActionNotSupported`
  ("Optional action not implemented") on the Action Service.
- **No `Schedule.*` or `Event.*` parameter group**; `param.cgi
  group=Schedule` returns `Error -1`.
- **`<CompatibleOsVersions>` field is real and used by shipping apps:**
  - `objectanalytics` declares `<Min>12</Min><Max>12</Max>`,
  - `vmd` declares `<Min>12.10</Min><Max>13</Max>`,
  - older apps without the field appear to install on any OS (implicit
    "any version").
- **Prior-art ACAP installed**: `Timelapse2` by Fred Juhlin
  (`/local/timelapse2/`). It exposes a Location tab + sunevents.html and
  publishes a `sunnoon` ACAP event topic. This is concrete proof that the
  "publish ACAP event topics for solar moments" model works on this
  platform.

## Camera 2 (AXIS OS 11 reference)

- **Firmware version**: `11.10.83` (build date 2024-05-29; AXIS OS 2024 LTS
  branch).
- **SoC**: Axis Artpec-7. **Architecture: `armv7hf`.**
- Geolocation API present; no location currently set
  (`ValidPosition=false`).
- Time API present; same TZ behavior as Camera 1.
- SOAP Action Service reachable; `GetActionRules` works.
- `/config/rest/scheduled-events/v{1,2}` not present.
- No schedule-related parameters in the global `param.cgi` dump.

## Key implications for the requirements

### Architecture mapping (PR-1 / PR-2) is partially wrong

The earlier requirement that "AXIS OS 12 = aarch64 only" is **false** for
the Artpec-7 generation. Both lab cameras run OS 12 (and OS 11) on
**armv7hf**. The build matrix must therefore produce armv7hf and aarch64
artifacts for **both** OS targets, not split arch by OS major version.
[21-platform-compatibility.md](./21-platform-compatibility.md) has been
updated.

### Schedule injection strategy: replaced

The originally specified injection point — REST Event Schedule API at
`/config/rest/scheduled-events/v2` — does not exist on shipping firmware.
**Resolved by [DL-05](./28-decision-log.md):** Path A (publish ACAP
event topics) replaces it. See
[FR-8](./08-event-registration.md) for the registration mechanism and
[FR-9](./09-event-firing.md) for the firing mechanism. The old
iCalendar / REST-write surface is gone from the requirements.

### Manifest OS compatibility (OQ-1)

- The earlier "no documented manifest field for minimum OS version" finding
  was wrong. The manifest field is `compatibleOsVersions` with
  `versionRange` entries (Min/Max). Both major-only (`Min=12 Max=12`) and
  minor-precision (`Min=12.10 Max=13`) ranges are honored. v11-only `.eap`
  builds can declare `Min=11 Max=11`; v12 builds `Min=12 Max=13` (or
  whatever Axis's current ceiling is at release time).

### Timezone (OQ-2 partial)

- AXIS OS surfaces a current IANA zoneinfo and applies DST correctly. The
  remaining unknown — whether the ACAP v12 sandbox can read
  `/usr/share/zoneinfo/...` — must be confirmed by an installed-app
  smoke test (a one-line `localtime_r()` call in a stub ACAP). Until that
  test runs, FR-2's "rely on system zoneinfo" stance is unchanged but
  flagged.

## Probe methodology (for reproducibility)

All probes were read-only HTTPS calls with HTTP Digest auth. Examples:

```sh
# firmware version
curl -sk --anyauth -u "root:$AXIS_PASS" \
  "https://$CAMERA_IP/axis-cgi/param.cgi?action=list&group=Properties.Firmware"

# geolocation
curl -sk --anyauth -u "root:$AXIS_PASS" \
  "https://$CAMERA_IP/axis-cgi/geolocation/get.cgi"

# time/timezone (POST JSON)
curl -sk --anyauth -u "root:$AXIS_PASS" -X POST \
  -H 'Content-Type: application/json' \
  -d '{"apiVersion":"1.0","method":"getDateTimeInfo"}' \
  "https://$CAMERA_IP/axis-cgi/time.cgi"

# SOAP Action Service GetActionRules
curl -sk --anyauth -u "root:$AXIS_PASS" -X POST \
  -H 'Content-Type: application/soap+xml; action="http://www.axis.com/vapix/ws/action1/GetActionRules"' \
  -d '<?xml version="1.0"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:act="http://www.axis.com/vapix/ws/action1"><s:Body><act:GetActionRules/></s:Body></s:Envelope>' \
  "https://$CAMERA_IP/vapix/services"

# installed apps (XML)
curl -sk --anyauth -u "root:$AXIS_PASS" \
  "https://$CAMERA_IP/axis-cgi/applications/list.cgi"
```

`$AXIS_PASS` and `$CAMERA_IP` are sourced from environment / local memory,
**never committed**.
