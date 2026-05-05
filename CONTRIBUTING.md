# Contributing

Camera_Schedule is MIT-licensed and developed in public. Contributions
of any size are welcome.

This file is a placeholder during early development. Full contributor
guidance — DCO sign-off requirements, branch protection rules, the
review checklist — will land in **M8** (see
[`IMPLEMENTATION.md`](./IMPLEMENTATION.md)).

Until then, the working rules are:

## Before you open a PR

1. Read the relevant requirement file under [`requirements/`](./requirements/).
2. Read [`requirements/28-decision-log.md`](./requirements/28-decision-log.md)
   to confirm the change you're proposing isn't already decided against.
3. If your change touches a milestone that's not yet underway, open an
   issue first to discuss sequencing.

## Code expectations

- C99, no compiler warnings under the project's `clang-tidy` ruleset
  (added in M1).
- Every project-owned source file SHALL carry an `SPDX-License-Identifier: MIT`
  header.
- Vendored files keep their original copyright lines; modifications get a
  `// Modified for Camera_Schedule, 2026:` block at the change site
  (see [`requirements/27-reuse-from-timelapse2.md`](./requirements/27-reuse-from-timelapse2.md)).
- No external CDN loads from the web UI. All assets ship in `app/html/`
  ([DL-08](./requirements/28-decision-log.md)).

## Decision-log discipline

The decision log is **append-only**. If your change reverses or refines
a prior decision, append a new `DL-NN` entry that says "supersedes
DL-NN" — do not edit the original.

## Testing

Until M1 lands the build pipeline, there are no automated tests. After M1:

- `make build-all` from `app/` SHALL succeed.
- Host-side fixtures under `app/test/host/` SHALL pass on x86_64 Linux
  without an emulator (BR-3).

## Lab cameras

The two test cameras and their root credentials are not part of this
repo and never will be (see
[`requirements/28-decision-log.md`](./requirements/28-decision-log.md) DL-09).
Probe scripts SHALL read camera addresses and credentials from
environment variables.

## Reporting security issues

See [`SECURITY.md`](./SECURITY.md).
