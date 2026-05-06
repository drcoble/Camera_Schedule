# Verification

End-to-end acceptance checks the implementation MUST satisfy before release.

## Test environment

The project has access to a dedicated lab with two reference cameras,
both supported integration targets under the
[DL-15](./28-decision-log.md) AXIS OS 11.11+ floor:

- a camera running **AXIS OS 12.10.61** (Artpec-7 / armv7hf) — the
  primary OS-12 integration target,
- a camera running **AXIS OS 11.11.192** (Artpec-7 / armv7hf) — the
  lower-bound OS-11.11+ integration target.

Both cameras run armv7hf, so they exercise the same `.eap` artifact;
aarch64 coverage requires bringing in an Artpec-8+ unit before
promotion from `v1.0.0-beta` to `v1.0.0` GA
([PR-6](./21-platform-compatibility.md),
[DL-23](./28-decision-log.md)). The aarch64 `.eap` is still built
on every change by CI — only its hardware smoke test is deferred.

Camera addresses and root credentials are tracked in the developer's
local Claude memory and SHALL NOT be checked into the public repository
([25-licensing-and-distribution.md](./25-licensing-and-distribution.md)).
Probe scripts and CI integration tests SHALL accept camera addresses and
credentials via environment variables, never as in-tree literals.

## Acceptance checks

## Acceptance checks

1. **Build.** `make build-armv7hf build-aarch64` produces two `.eap`
   files under the project's CI image. Each is ≤ 5 MB
   ([NFR-1](./20-non-functional.md)). Artifacts are unsigned for
   the `v1.0.0-beta` tag per [DL-23](./28-decision-log.md); signing
   is reopened post-beta.

2. **Install on device.**
   - `eap-install.sh install && eap-install.sh start` succeeds on the
     OS 12 lab camera (armv7hf) and on any OS 11.11+ camera available
     for testing.
   - The configuration UI is reachable in the camera's web interface and
     loads its assets without any external network requests
     ([FR-11.3](./11-configuration-ui.md)).

3. **Location pickup.**
   - Setting lat/lon via `/axis-cgi/geolocation/set.cgi` is reflected in the
     app UI within one poll interval (default 1 h; reduce for the test).
   - A configured manual override takes precedence over the camera's stored
     value ([FR-1.3](./01-geo-location.md)).

4. **Schedule generation end-to-end.**
   - Configure an anchor "30 min before sunrise" bound to an action rule
     that publishes an MQTT message.
   - Observe MQTT events firing at the correct local time across at least
     **7 consecutive days**, including at least one DST boundary if
     available (otherwise, validate DST behavior in the host harness).

5. **Polar safety.**
   - Host-side fixtures for **lat = 78° N** across the winter solstice
     show no malformed schedules emitted, the expected
     `NO_EVENT_TODAY` log entries, and solar midnight events still
     present ([FR-3.8](./03-solar-events.md)).

6. **Topic registration & reconciliation.**
   - On install, the app's anchors appear as event topics under
     `tnsaxis:CameraApplicationPlatform/<appName>/...` in the camera's
     Action Rules UI ([FR-8](./08-event-registration.md)).
   - Adding/removing/renaming an anchor in the app UI is reflected in
     the Action Rules topic list within one recompute cycle
     ([FR-8.5](./08-event-registration.md)).
   - Action rules created by the operator that reference these topics
     continue to work (no flapping) when an anchor is renamed without
     changing its `id`.

7. **Upgrade.**
   - Reinstalling a newer `.eap` preserves the JSON config in `localdata/`.
   - The first post-upgrade boot is idempotent: re-declaring already-known
     topics is a no-op ([FR-8.6](./08-event-registration.md)) and bound
     action rules survive.

8. **Sandbox compliance.**
   - The app runs as a generated dynamic non-root user on every
     supported OS target.
   - On the OS 12 reference camera, `apparmor_status` shows no policy
     violations during a 24-hour soak test
     ([NFR-4](./20-non-functional.md)).

9. **License audit.**
   - A bill-of-materials audit on the release `.eap` confirms no LGPL,
     GPL, or noncommercial-restricted code is bundled
     ([NFR-6](./20-non-functional.md)).

10. **UI accessibility & isolation.**
    - All configuration controls are keyboard-reachable
      ([FR-11.4](./11-configuration-ui.md)).
    - UI loads with the camera's outbound network blocked at the firewall —
      no broken assets, no console errors.
