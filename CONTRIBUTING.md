# Contributing

Camera_Schedule is MIT-licensed and developed in public. Contributions
of any size are welcome.

This guide covers what you need to build, test, and submit a change.
The architecture is fixed by the [requirements](./requirements/) and
the [decision log](./requirements/28-decision-log.md) — read both
before proposing a substantive change.

## Building locally

The build is fully Dockerized. The only host-side requirements are:

- Docker with Buildx support
- GNU `make`

Everything else (the cross-compile toolchain, the ACAP Native SDK,
the manifest validator, and the `acap-build` packaging tool) runs
inside the pinned `axisecp/acap-native-sdk:12.6.0-${ARCH}-ubuntu24.04`
image.

From the repository root:

```sh
make -C app build-armv7hf   # OS 11.11+ and OS 12 on Artpec-7
make -C app build-aarch64   # OS 12 on Artpec-8+
make -C app build-all       # both
make -C app help            # full target menu
```

Outputs land in `dist/`:

```
dist/camera-schedule-armv7hf.eap
dist/camera-schedule-aarch64.eap
```

When you add a file under `app/` outside `html/` or `lib/` that needs
to ship in the `.eap`, add a `-a path/to/file` argument to the
`acap-build` invocation in `app/Dockerfile` and verify with `tar tzf`
on the produced artifact. `acap-build` silently drops files that it
doesn't recognize as well-known. See the "Adding files to the .eap"
section in [`CLAUDE.md`](./CLAUDE.md) for the full rationale.

## Testing

### Host-side fixtures

Host fixtures live under `app/test/host/` and cover the astronomy
math, anchor expansion, calendar parsing, and config export/import
round-trip. They link only against `cJSON` and the standard C
library, so they run on any x86_64 Linux without an emulator
([BR-3](./requirements/22-build-and-packaging.md)):

```sh
make -C app test               # all host fixtures
make -C app test-lunar         # lunar suite only
make -C app test-seasonal      # seasonal suite only
make -C app test-anchors       # anchors suite only
make -C app test-calendar      # calendar suite only
make -C app test-export-import # export/import round-trip
```

A change to astronomy code, anchor logic, or persistence formats
SHALL include a host fixture covering the new behavior.

### Lab integration

Integration scripts live under `app/test/lab/`. They run against
real cameras and read endpoint addresses and credentials from
environment variables — never from in-tree files (per
[DL-09](./requirements/28-decision-log.md)):

```sh
export AXIS_HOST_OS12=...   # Artpec-7 / OS 12 lab camera
export AXIS_HOST_OS11=...   # Artpec-7 / OS 11.11+ lab camera
export AXIS_PASS=...
```

The 24-hour soak harness (`app/test/lab/soak_24h.sh`) is the
post-tag gating step before a release: it samples
`/state.rss_kb`, AppArmor `system_log.cgi`, and recompute errors
every 60 s for 24 hours.

The Artpec-8+ / aarch64 build is verified by CI but has no lab
target yet ([DL-23](./requirements/28-decision-log.md)).

## Pull-request workflow

1. Branch off `main`. Use a short, lowercase, hyphen-separated
   branch name (`fix-anchor-dst`, `m8/license-audit`).
2. Read the relevant requirement file under
   [`requirements/`](./requirements/) and confirm the decision log
   does not already rule against the change.
3. If the change touches a milestone that is not yet underway,
   open an issue first to discuss sequencing.
4. Make small, atomic commits. Each commit should leave the tree
   in a buildable state.
5. Push the branch and open a pull request against `main`.
6. The CI matrix (build × {armv7hf, aarch64} + manifest sanity +
   `make help` + host fixtures) MUST be green before review.
7. Squash-merge is allowed; merge-commit is allowed; force-push to
   `main` is forbidden by branch protection. Tagged release
   history is never rewritten ([DR-4](./requirements/25-licensing-and-distribution.md)).

## Commit-message conventions

Commits use a short semantic prefix followed by an imperative
subject under ~70 characters. Body lines wrap at ~72 characters.

Prefixes in current use, derived from the existing history:

| Prefix | When |
|---|---|
| `M[0-9]:` | Net-new milestone work (e.g. `M6: anchors, calendar, schedule UI`) |
| `M[0-9] <phase>:` | Sub-phase of a milestone (`M7 SSE: status ring buffer`, `M7 UI: status panel`) |
| `fix:` | Bug fix |
| `fix(<scope>):` | Scoped bug fix (`fix(M7): rename /status → /state`) |
| `docs:` | Documentation-only change |
| `ci:` | CI / GitHub Actions change |
| `build:` | Makefile, Dockerfile, packaging |
| `manifest:` | `app/manifest.json` change |

Reference requirement clauses (`FR-7.4`, `NFR-6`) and decision-log
entries (`DL-18`) directly in the subject or body when the commit
implements one.

## Developer Certificate of Origin (DCO)

Contributions are accepted under the
[Developer Certificate of Origin v1.1](https://developercertificate.org/).
Every commit MUST carry a `Signed-off-by:` trailer attesting that
the author has the right to submit the work under the project's
MIT license. Sign off with:

```sh
git commit -s -m "fix: correct anchor DST rollover"
```

which appends:

```
Signed-off-by: Your Name <your.email@example.com>
```

The author name and email in the trailer MUST match `git config
user.name` and `git config user.email`. Anonymous or pseudonymous
contributions are not accepted; the email address must be one you
control. To re-sign a stack of existing commits, use
`git rebase --signoff`.

DCO is enforced as policy, not as a CI check, for v1.0.0-beta. A
PR with unsigned commits will be asked to re-submit before merge.

## Code style

### C

- C99, no compiler warnings under the project's build flags.
- 4-space indentation, no tabs.
- `snake_case` for functions, variables, file names; `UPPER_SNAKE_CASE`
  for `#define` constants.
- One declaration per line. Braces on the same line as the
  controlling statement (`if (cond) {`).
- Match the vendored Timelapse2 style for any code that touches the
  ACAP framework surface — that codebase is the reference for
  framework-adjacent idioms ([DL-06](./requirements/28-decision-log.md)).
- Every project-owned source file SHALL carry an
  `SPDX-License-Identifier: MIT` header
  ([DR-2](./requirements/25-licensing-and-distribution.md)).
- Vendored files keep their original copyright lines verbatim;
  modifications get a `// Modified for Camera_Schedule, 2026:`
  block at the change site
  ([27-reuse-from-timelapse2.md](./requirements/27-reuse-from-timelapse2.md)).

### JavaScript

- Vanilla JS only. No frameworks, no transpilation, no build step
  for the UI.
- No external CDN loads. All assets ship in `app/html/`
  ([DL-08](./requirements/28-decision-log.md)).
- 2-space indentation, single quotes for strings, semicolons
  required.
- ES2017 syntax floor (matches what the camera's Chromium-based
  embedded UI runtime supports).

### HTML

- Keyboard-reachable controls and ARIA labelling per
  [FR-11.4](./requirements/11-configuration-ui.md).

## Decision-log discipline

The [decision log](./requirements/28-decision-log.md) is
**append-only**. If a change reverses or refines a prior decision,
append a new `DL-NN` entry that says "supersedes DL-NN" and update
only the earlier entry's status to `superseded by DL-NN`. Do not
edit existing entries.

When you remove or materially change a requirement, capture the
removal in a new DL entry. The "Removed / changed" block is what
future contributors will rely on to understand why something looks
the way it does.

## Where to file issues

- **Bugs and feature requests**: the public GitHub issue tracker at
  <https://github.com/drcoble/Camera_Schedule/issues>.
- **Security issues**: do not open a public issue. Follow the
  private-disclosure path documented in [`SECURITY.md`](./SECURITY.md).

## Lab cameras

The two test cameras and their root credentials are not part of
this repo and never will be ([DL-09](./requirements/28-decision-log.md)).
Probe scripts SHALL read camera addresses and credentials from
environment variables.
