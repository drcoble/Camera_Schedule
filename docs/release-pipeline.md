# Release pipeline

Single reference for the M8 CI/release machinery. Aimed at future
contributors who need to extend, debug, or operate the pipeline.
Decisions are append-only in
[`requirements/28-decision-log.md`](../requirements/28-decision-log.md);
see DL-25 (license audit), DL-26 (reproducible build), and
DL-27 (release publish) for rationale on choices made here.

## Overview

Three workflows under [`.github/workflows/`](../.github/workflows)
implement the M8 pipeline:

| Workflow | Trigger | What it does |
|---|---|---|
| `ci.yml` (pre-existing) | push to `main`, PR | Builds the `.eap` matrix (armv7hf + aarch64), runs host-fixture tests, manifest sanity, `make help` |
| `license-audit.yml` | push to `main`, PR | Runs `app/scripts/license_audit.py`; fails on a non-allowlist license |
| `reproducibility.yml` | push to `main`, PR | Builds each architecture twice and asserts SHA-256 identity |
| `release.yml` | tag push matching `v[0-9]+.[0-9]+.[0-9]+(-beta)?`; also `workflow_dispatch` for dry-run | Builds determinist `.eap` artifacts with the SDK image digest pinned, generates `SHA-256SUMS.txt`, drafts a GitHub Release with the assets attached |

Every push and PR therefore exercises the build matrix, the license
audit, and the reproducibility check. Tag pushes additionally fire
the release workflow.

## License audit (DL-25, NFR-6, DR-11)

The audit is a hand-rolled stdlib-only Python script at
[`app/scripts/license_audit.py`](../app/scripts/license_audit.py).
It walks the `.eap`-bundled file tree, classifies each file by SPDX
header (project-owned C/headers) or by an in-script manifest
(vendored third-party files), and fails non-zero if any classified
license is not on the allowlist or any file under `app/src/` /
`app/settings/` is unclassifiable.

**Allowlist** ([`approved-licenses.txt`](../approved-licenses.txt)):
MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, ISC, 0BSD,
Unlicense, CC0-1.0. LGPL and GPL are deliberately excluded —
adding either requires the `lgpl-relink/` release-pipeline path
described in NFR-6 and a new DL entry justifying the relink
commitment.

### Adding a new bundled dependency

1. Add the file under `app/src/` (or wherever it belongs).
2. If project-owned, ensure the SPDX header line is present:
   `// SPDX-License-Identifier: MIT`. If vendored, preserve the
   upstream header verbatim.
3. Update [`THIRD_PARTY_LICENSES.md`](../THIRD_PARTY_LICENSES.md)
   with the component name, version, license, and full license
   text.
4. If the license is not already on the allowlist, append a
   DL-NN entry justifying the addition, then add the SPDX
   identifier to `approved-licenses.txt`.
5. Run `make -C app license-audit` locally to confirm the gate
   passes.

### Failure modes

The script catches three:

1. A new file with a non-allowlist SPDX header (e.g. an LGPL drop
   into `app/src/`).
2. An undocumented vendored file (no SPDX header, not in the
   script's `VENDORED_FILES` manifest).
3. Inventory drift between `THIRD_PARTY_LICENSES.md` and the file
   tree.

### Synthetic-PR test

The gate is end-to-end verifiable by introducing a single source
file with a non-allowlist SPDX header (e.g.
`// SPDX-License-Identifier: LGPL-2.1-or-later`) on a branch and
opening a PR. The `License audit` job MUST report `fail` within
seconds. M8 ran this test as PR #2 (closed, branch deleted); CI
run `25524553292` confirmed the gate works.

## Reproducible build (DL-26, BR-6)

Three pieces of plumbing achieve byte-identical `.eap` SHA-256
between two clean checkouts of the same source tree:

1. **`SOURCE_DATE_EPOCH`** is defaulted from the head commit's
   author timestamp by the Makefile
   (`SOURCE_DATE_EPOCH ?= $(git log -1 --pretty=%ct)`). CI exports
   the same value before invoking `make`.
2. **`LC_ALL=C`** is exported by the Makefile, pinning `sort` and
   other locale-sensitive utilities to the C collation.
3. **[`app/scripts/repack_eap.sh`](../app/scripts/repack_eap.sh)**
   rewrites the `.eap` after `acap-build`. The `.eap` is a gzip'd
   tar; the repack re-extracts, normalizes file mtimes to
   `SOURCE_DATE_EPOCH`, re-tars with
   `--sort=name --owner=0 --group=0 --numeric-owner --mtime=@SDE
   --format=ustar`, and recompresses with `gzip -n9` (`-n` strips
   the original-filename and mtime fields per RFC 1952 §2.3.1).

The compiled ELF binary inside the `.eap` is reproducible *given*
the SDK image is bit-stable across the two builds:

- The linker flag `-s` (in `LDLIBS` since v0.1.0) strips the
  binary, removing DWARF debug-info timestamps and randomized
  ELF build-ids.
- The project sources do not use `__DATE__`, `__TIME__`, or
  `__FILE__` patterns that would leak absolute paths or build
  times (verified by `grep -r '__DATE__\|__TIME__' app/src/` =
  empty).

### SDK image digest pinning

The Dockerfile accepts an optional `SDK_DIGEST` build-arg. When
non-empty, the `FROM` line resolves to
`axisecp/acap-native-sdk:<tag>@<digest>`, pinning the image
immutably. When empty (default for dev builds), it falls back to
the floating tag — acceptable because both runs of the
reproducibility CI job use the same already-pulled local image
within a single workflow run.

For releases, the digest is resolved automatically by
`release.yml` via `docker buildx imagetools inspect …` and
threaded into the build through the per-arch Makefile vars
`SDK_DIGEST_armv7hf` / `SDK_DIGEST_aarch64`.

For ad-hoc local reproducibility checks, pin manually:

```sh
SDK_DIGEST_armv7hf=sha256:abc... make -C app build-armv7hf
```

### `make reproducibility-check`

A local Makefile target builds each architecture twice and asserts
SHA-256 identity. Use it before opening a PR that touches build
plumbing.

## Release publish (DL-27, DR-12)

`release.yml` fires on:

- **Tag push** matching `v[0-9]+.[0-9]+.[0-9]+(-beta)?` —
  produces a real (drafted) GitHub Release.
- **`workflow_dispatch`** with `dry_run=true` (default) — same
  build + assemble path, but skips `gh release create`. Used to
  verify the assembly works before pushing the actual tag.

### Asset bundle

Each release attaches:

- `camera-schedule-armv7hf.eap` — armv7hf artifact (Artpec-7,
  OS 11.11+ and OS 12.x)
- `camera-schedule-aarch64.eap` — aarch64 artifact (Artpec-8+,
  OS 12.x; built but not lab-smoked for v1.0.0-beta per DL-23)
- `SHA-256SUMS.txt` — combined SHA-256 manifest for both `.eap`
- `THIRD_PARTY_LICENSES.md` — bundled-license inventory
- `CHANGELOG.md` — full project changelog

### Release notes body

Auto-extracted from the `CHANGELOG.md` section matching the tag
by [`app/scripts/extract_release_notes.py`](../app/scripts/extract_release_notes.py).
The script accepts `--tag` and `--changelog`; if the matching
section is missing, it falls back to a one-line placeholder so the
workflow does not fail. The integrator can amend the draft body
before un-drafting.

### Always-draft policy

Every release is created with `--draft` unconditionally. Promotion
from draft to public is a manual integrator step:

```sh
gh release edit v1.0.0-beta --draft=false --repo drcoble/Camera_Schedule
```

Three reasons (DL-27): (1) asset review before publication,
(2) CHANGELOG synchronization headroom, (3) mistake recovery on a
fat-fingered tag push.

### Pre-release flag

Tags ending in `-beta` get `--prerelease`. Tags without a suffix
publish as a full release once un-drafted.

## Cutting a release

For the next maintainer who needs to ship a tag:

1. **Pre-flight**: confirm CI green on `main` HEAD; confirm soak
   evidence (NFR-4) is fresh; confirm verification report at
   `docs/verification/<tag>-readiness.md` exists and aggregates
   PASS or PARTIAL (no FAIL).
2. **Advisor checkpoint**: a tag is hard to reverse on a public
   repo. Run an advisor pass against the planned tag's
   commit-range, release-notes preview, and any open issues.
3. **(Optional) dry-run**: trigger `release.yml` via
   `gh workflow run release.yml --ref main -f tag=<TAG> -f dry_run=true`.
   Confirm both `.eap` build and the asset bundle is correct.
4. **Tag annotated**:
   ```sh
   git tag -a v1.0.0-beta -m "Camera_Schedule v1.0.0-beta — first public 1.x release"
   git push origin v1.0.0-beta
   ```
5. **Wait for `release.yml`** — typically 4–6 minutes. Verify in
   the Actions tab.
6. **Review the draft Release** — open the URL in the workflow
   summary. Confirm: tag, prerelease flag (if `-beta`), all five
   assets present, body extracted from CHANGELOG.
7. **Un-draft**:
   ```sh
   gh release edit v1.0.0-beta --draft=false --repo drcoble/Camera_Schedule
   ```
8. **Post-tag**: update `CLAUDE.md` repository-state section to
   reflect the new tag; update `IMPLEMENTATION.md` only if the
   milestone definition itself changed.

## Where things live

| Concern | Path |
|---|---|
| License audit script | [`app/scripts/license_audit.py`](../app/scripts/license_audit.py) |
| License allowlist | [`approved-licenses.txt`](../approved-licenses.txt) |
| Bundled-license inventory | [`THIRD_PARTY_LICENSES.md`](../THIRD_PARTY_LICENSES.md) |
| `.eap` repack (reproducibility) | [`app/scripts/repack_eap.sh`](../app/scripts/repack_eap.sh) |
| Release-notes extractor | [`app/scripts/extract_release_notes.py`](../app/scripts/extract_release_notes.py) |
| Build determinism Makefile vars | [`app/Makefile`](../app/Makefile) (`SOURCE_DATE_EPOCH`, `LC_ALL`, `SDK_DIGEST_*`) |
| SDK image pin | [`app/Dockerfile`](../app/Dockerfile) (`ARG SDK_DIGEST=`) |
| Local repro check | `make -C app reproducibility-check` |
| Local audit check | `make -C app license-audit` |
| Decision rationale | DL-25, DL-26, DL-27 in [`requirements/28-decision-log.md`](../requirements/28-decision-log.md) |
