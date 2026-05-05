# Security policy

## Reporting a vulnerability

If you believe you've found a security issue in Camera_Schedule:

1. **Do not** open a public GitHub issue.
2. Open a private security advisory via the repository's GitHub
   "Security" tab → "Report a vulnerability."

We aim to acknowledge reports within five business days and to ship a
fix or mitigation in the next tagged release.

## Threat model

Camera_Schedule runs **on-camera** as a signed ACAP application under
the AXIS OS dynamic-user sandbox (no root). Within that environment its
attack surface is:

- One or more FastCGI endpoints under `/local/<appName>/` reachable by
  authenticated camera users (per the manifest's `httpConfig.access`
  field).
- D-Bus calls to `com.axis.HTTPConf1.VAPIXServiceAccounts1.GetCredentials`
  to obtain VAPIX credentials for the geolocation read/write path.
- Local file I/O under the ACAP-allocated `localdata/` directory.

The app does **not**:

- Open inbound network sockets.
- Read or write the camera's video pipeline.
- Require root.
- Bundle any tzdata, holiday, or third-party astronomical database that
  could become a stale-data vector.

## Signing-key handling

The project's Axis Application Signing key is held only as a GitHub
Actions secret on the protected `main` branch ([DL-11](./requirements/28-decision-log.md)).
Signing runs only on tag pushes; PR builds are unsigned. Key rotation
is planned yearly or on suspected compromise. The key never enters
contributor machines, fork CI, or the source tree.

## Disclosure timeline

We follow coordinated disclosure: report → triage → fix → release →
public advisory. Embargoes longer than 90 days are negotiated case by
case.
