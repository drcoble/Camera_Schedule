# Open questions / risks

All questions raised during requirements elicitation, hardware probing,
and the Timelapse2 review have been resolved. This file is the closed-OQ
ledger; full rationale and any cascading edits live in
[28-decision-log.md](./28-decision-log.md).

If a new uncertainty surfaces during implementation, append it here as
**OQ-12** (or higher), then resolve via a new decision-log entry rather
than amending this list in place.

> **Test hardware in use.** Lab AXIS OS 12.10.61 and OS 11.10.83 cameras
> are reachable on the development network; addresses and credentials
> live in the developer's local Claude memory and **never** in this
> repository. Scripts that hit the cameras SHALL read credentials from
> environment variables or a local `.env` outside the repository tree.

## Closed

- ~~**OQ-1** — Manifest minimum-OS declaration~~ → resolved by probe.
  Field is `compatibleOsVersions: [{ versionRange: { min, max } }]`.
  See [DL-04](./28-decision-log.md).
- ~~**OQ-2** — tzdata inside the ACAP sandbox~~ → resolved positive.
  Timelapse2's localized sunrise/sunset on the v12 lab camera proves
  zoneinfo is readable. See [DL-10](./28-decision-log.md).
- ~~**OQ-3** — Application-signing workflow~~ → GitHub Actions on
  tagged main, key in repo secrets. See [DL-11](./28-decision-log.md)
  and [BR-7](./22-build-and-packaging.md).
- ~~**OQ-4** — On-board GPS cameras~~ → out of scope for v1.
  See [DL-12](./28-decision-log.md).
- ~~**OQ-5** — Event Schedule API quotas~~ → moot. Path A doesn't use
  the API. See [DL-05](./28-decision-log.md).
- ~~**OQ-6** — VAPIX Event Schedule API~~ → resolved negatively. API
  not present on shipping firmware. See [DL-05](./28-decision-log.md).
- ~~**OQ-7** — Reverse-proxy auth role propagation on v12~~ → works
  via manifest `httpConfig.access`. See [DL-13](./28-decision-log.md).
- ~~**OQ-8** — Anchor cap~~ → 64 locked for v1.
  See [DL-14](./28-decision-log.md).
- ~~**OQ-9** — Project license~~ → MIT.
  See [DL-01](./28-decision-log.md).
- ~~**OQ-10** — Schedule-injection architecture~~ → Path A (publish
  ACAP event topics). See [DL-05](./28-decision-log.md).
- ~~**OQ-11** — OS floor and build matrix~~ → AXIS OS 11.11+ floor;
  build matrix simplified to two `.eap` artifacts.
  See [DL-15](./28-decision-log.md).
- ~~**OQ-12** — `compatibleOsVersions` not in any SDK 12.6.0 schema~~ →
  field doesn't exist in `application-manifest-schema-v1.0` through
  `v1.8.0` (verified by inspecting the schemas bundled in
  `axisecp/acap-native-sdk:12.6.0`). Field dropped from the manifest;
  PR-1 amended. See [DL-16](./28-decision-log.md).
