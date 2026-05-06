# Licensing & distribution

The project is open source. This file captures requirements that flow from
that decision: project license selection, source-code availability,
contribution model, public release channel, and third-party attribution.

## Project license

- **DR-1 — Project license: MIT.** The repository SHALL declare the
  **MIT License** at the root in `LICENSE`. Rationale:
  - shortest and simplest OSI-approved license text,
  - maximally permissive: any downstream use (commercial, proprietary,
    statically-linked redistribution) is allowed with only an
    attribution / license-text retention obligation,
  - universally recognized, trivially compatible with every other
    permissive license and with both LGPL and GPL downstream.

- **DR-2 — License headers.** Every source file under the project's own
  copyright SHALL carry an SPDX short-form header
  (e.g. `// SPDX-License-Identifier: MIT`). Files imported from third-party
  permissive sources SHALL preserve their original headers.

- **DR-3 — `LICENSE`.** The repository root SHALL contain a `LICENSE` file
  with the full MIT License text and the project's copyright line. MIT does
  not require a separate `NOTICE` file; bundled-dependency attributions
  belong in `THIRD_PARTY_LICENSES.md` ([DR-10](#dr-10-license-inventory)).

## Source code availability

- **DR-4 — Public repository.** The canonical source SHALL live in a public
  repository (GitHub or equivalent). All development history SHALL be
  visible; no squash-rewrites of the public history after release tags.

- **DR-5 — No private build path.** It SHALL be possible to build the
  release `.eap` from the public source tree using only the publicly
  available ACAP Native SDK Docker images, with no proprietary tooling or
  closed-source build steps required. Signing
  ([BR-7](./22-build-and-packaging.md)) is the documented exception — the
  signing key is private; an unsigned but functionally identical build
  SHALL be reproducible from public sources.

- **DR-6 — Issue tracker & roadmap.** Bug reports, feature requests, and
  roadmap items SHALL be tracked in the public repository's issue tracker.
  Security-sensitive reports SHALL have a documented private channel
  (DR-9).

## Contribution model

- **DR-7 — `CONTRIBUTING.md`.** The repository SHALL include a
  `CONTRIBUTING.md` describing:
  - how to build and test locally
    ([22-build-and-packaging.md](./22-build-and-packaging.md)),
  - the pull-request workflow and review expectations,
  - the expected commit-message and code-style conventions,
  - the contributor sign-off / DCO posture (recommended:
    `Signed-off-by:` per the Developer Certificate of Origin v1.1).

- **DR-8 — `CODE_OF_CONDUCT.md`.** The repository SHALL include a code of
  conduct (Contributor Covenant or equivalent) and name an enforcement
  contact.

- **DR-9 — Security policy.** The repository SHALL include `SECURITY.md`
  describing the private disclosure channel (email or GitHub private
  vulnerability reporting), the expected response window, and the
  supported-versions table.

## Third-party attribution

- **DR-10 — License inventory.** The build SHALL produce a
  third-party-license inventory file (`THIRD_PARTY_LICENSES.md` or similar)
  enumerating every bundled third-party component, its version, its
  license, and a copy of its license text. This file SHALL be included in
  the release-artifact bundle ([BR-8](./22-build-and-packaging.md)).

- **DR-11 — License audit in CI.** CI SHALL fail the build if a bundled
  dependency's license is not on the project's approved list
  ([NFR-6](./20-non-functional.md)). The approved list SHALL be checked
  into the repository.

## Public release channel

- **DR-12 — Tagged releases.** Releases SHALL be cut as annotated git tags
  (`vMAJOR.MINOR.PATCH`, with semver pre-release suffix where applicable —
  e.g. `v1.0.0-beta` per [DL-23](./28-decision-log.md)) and SHALL be
  accompanied by a public release page containing:
  - the armv7hf and aarch64 `.eap` artifacts (signed per BR-7 once the
    signing key is in hand; unsigned for `v1.0.0-beta` per DL-23),
  - SHA-256 checksums of each artifact,
  - the `THIRD_PARTY_LICENSES.md` bundle (DR-10),
  - a CHANGELOG entry,
  - the `lgpl-relink/` directory if any LGPL code is bundled
    ([NFR-6](./20-non-functional.md)).

- **DR-13 — Versioning.** The project SHALL follow Semantic Versioning 2.0.
  Manifest `version` field values SHALL track tag values.

- **DR-14 — ACAP Share / marketplace listing (optional).** Listing the
  app on the Axis ACAP Share or a similar marketplace is encouraged but
  not required by these requirements.

## Notes

- "Open source" in these requirements means OSI-approved license, public
  source, and public bug tracker. It does **not** require donating
  copyright to a foundation, requiring contributor CLAs, or any specific
  governance model — those are project-management decisions outside the
  scope of these requirements.
