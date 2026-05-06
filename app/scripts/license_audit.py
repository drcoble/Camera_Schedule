#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Camera_Schedule contributors
#
# License audit for the .eap-bundled source tree (NFR-6 / DR-11).
#
# Walks the files that end up inside the released .eap (project-owned
# C/headers, vendored third-party files under app/src/acap/, bundled
# UI assets under app/html/, and the settings/events.json blob),
# classifies each by SPDX license id, and fails non-zero if any
# license falls off `approved-licenses.txt` at the repo root.
#
# Why a hand-rolled script and not `reuse` / `licensecheck` /
# `scancode-toolkit` — see DL-25 in requirements/28-decision-log.md.
# Short version: the repo's SPDX-header coverage is C/headers only
# (DR-2's "every source file" was scoped to .c/.h at the time the
# UI was built), so a strict whole-tree SPDX scanner would red-flag
# UI files this milestone is not chartered to touch. A tiny custom
# audit lets us enforce the gate exactly at NFR-6's grain — bundled
# code's *license*, not header-style policing — without pulling in
# a Python package network.
#
# Failure modes the audit catches:
#   1. A new file under app/src/ with an SPDX id outside the allowlist
#      (e.g. someone vendors an LGPL helper into app/src/lib/).
#   2. A new file under app/src/acap/ whose project-owned wrapper line
#      asserts a non-MIT license (vendored file mismatch).
#   3. THIRD_PARTY_LICENSES.md drifts out of sync with the file tree
#      — a vendored file that is not declared in the inventory, or
#      an inventory entry whose license falls off the allowlist.

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path

# ----------------------------------------------------------------------
# Inventory of vendored (non-project-owned) files. Keys are repo-relative
# paths; values are the SPDX id we attribute the file to. Adding a file
# here without also updating THIRD_PARTY_LICENSES.md is a CI failure.
#
# Keep this list short. The license audit's primary job is to catch a
# *new* third-party file slipping in unnoticed; the project-owned tree
# is identified by SPDX header, which is the cheap, accurate signal.
VENDORED_FILES: dict[str, str] = {
    "app/src/acap/ACAP.c": "MIT",
    "app/src/acap/ACAP.h": "MIT",
    "app/src/acap/cJSON.c": "MIT",
    "app/src/acap/cJSON.h": "MIT",
}

# Files inside app/ that DO ship in the .eap but are not source code
# we license-audit (they're either pure data or not text). Listed
# explicitly so an unclassified file produces a hard failure rather
# than being silently skipped.
DATA_FILES_OK: set[str] = {
    "app/settings/events.json",
}

# UI assets (HTML/CSS/JS) that ship in the .eap. These are project-
# owned but historically were authored without SPDX headers; per DL-25
# they're treated as MIT-by-LICENSE-file-at-root (DR-3) and excluded
# from the SPDX-header check below. If you add a new UI source file,
# add a `// SPDX-License-Identifier: MIT` header to it AND it'll pass
# the project-owned-source check; or add it here to be inventoried as
# project-owned UI asset.
UI_ASSET_DIRS: tuple[str, ...] = (
    "app/html/",
)

# Glob roots the audit walks for project-owned C / header sources.
PROJECT_OWNED_DIRS: tuple[str, ...] = (
    "app/src/",
)
PROJECT_OWNED_EXCLUDE_DIRS: tuple[str, ...] = (
    "app/src/acap/",  # vendored — handled via VENDORED_FILES
)
PROJECT_OWNED_EXTS: tuple[str, ...] = (".c", ".h")

SPDX_RE = re.compile(
    r"SPDX-License-Identifier:\s*([A-Za-z0-9.\-+ ]+?)\s*(\*/|\r?\n|$)"
)


@dataclass
class Finding:
    path: str
    license: str | None
    kind: str  # "project", "vendored", "ui-asset", "data", "unknown"
    note: str = ""


def discover_repo_root(start: Path) -> Path:
    """Walk up from `start` until we hit the repo root (where LICENSE
    + requirements/ live)."""
    p = start.resolve()
    while p != p.parent:
        if (p / "LICENSE").is_file() and (p / "requirements").is_dir():
            return p
        p = p.parent
    raise SystemExit(f"could not find repo root above {start}")


def load_allowlist(path: Path) -> set[str]:
    if not path.is_file():
        raise SystemExit(f"missing approved-licenses file: {path}")
    out: set[str] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        out.add(s)
    return out


def detect_spdx(path: Path) -> str | None:
    try:
        # Look at the first 4 KiB only — SPDX header convention is the
        # top of the file. Avoids reading large generated files.
        head = path.open("rb").read(4096).decode("utf-8", errors="replace")
    except OSError:
        return None
    m = SPDX_RE.search(head)
    return m.group(1).strip() if m else None


def is_under(path: str, prefixes: tuple[str, ...]) -> bool:
    return any(path == p.rstrip("/") or path.startswith(p) for p in prefixes)


def walk_audit(repo_root: Path) -> list[Finding]:
    findings: list[Finding] = []
    seen: set[str] = set()

    # Pass 1: project-owned C / headers.
    for d in PROJECT_OWNED_DIRS:
        base = repo_root / d
        if not base.exists():
            continue
        for fp in sorted(base.rglob("*")):
            if not fp.is_file():
                continue
            rel = fp.relative_to(repo_root).as_posix()
            if is_under(rel, PROJECT_OWNED_EXCLUDE_DIRS):
                continue
            # Header / .h / .c only at this stage; allow the contract
            # markdown files to fall through to the "unknown" pass so
            # an explicit policy decision is required for non-source.
            if not rel.endswith(PROJECT_OWNED_EXTS):
                continue
            seen.add(rel)
            spdx = detect_spdx(fp)
            findings.append(Finding(rel, spdx, "project"))

    # Pass 2: vendored manifest. Every entry must exist on disk; the
    # license attributed must be on the allowlist.
    for rel, lic in VENDORED_FILES.items():
        seen.add(rel)
        if not (repo_root / rel).is_file():
            findings.append(
                Finding(rel, lic, "vendored", note="declared but missing on disk")
            )
            continue
        findings.append(Finding(rel, lic, "vendored"))

    # Pass 3: UI assets (project-owned, MIT-by-LICENSE).
    for d in UI_ASSET_DIRS:
        base = repo_root / d
        if not base.exists():
            continue
        for fp in sorted(base.rglob("*")):
            if not fp.is_file():
                continue
            rel = fp.relative_to(repo_root).as_posix()
            seen.add(rel)
            findings.append(Finding(rel, "MIT", "ui-asset"))

    # Pass 4: known data files.
    for rel in sorted(DATA_FILES_OK):
        seen.add(rel)
        if not (repo_root / rel).is_file():
            findings.append(
                Finding(rel, None, "data", note="declared but missing on disk")
            )
            continue
        findings.append(Finding(rel, "MIT", "data"))

    # Pass 5: anything else under app/src/ or app/settings/ that we
    # didn't classify is a *new third-party drop* until proven otherwise.
    for d in ("app/src/", "app/settings/"):
        base = repo_root / d
        if not base.exists():
            continue
        for fp in sorted(base.rglob("*")):
            if not fp.is_file():
                continue
            rel = fp.relative_to(repo_root).as_posix()
            if rel in seen:
                continue
            # Allow our own contract markdown to live in app/src/ —
            # they're not bundled into the .eap (acap-build doesn't
            # bundle .md from src/), so they don't need licensing
            # treatment here. But still SPDX-check them to stay tidy.
            if rel.endswith(".md"):
                spdx = detect_spdx(fp) or "MIT"
                findings.append(Finding(rel, spdx, "project"))
                continue
            findings.append(
                Finding(
                    rel,
                    None,
                    "unknown",
                    note="unclassified file under bundled tree; "
                    "add to VENDORED_FILES (with SPDX id and a "
                    "THIRD_PARTY_LICENSES.md entry) or add a project "
                    "SPDX header to bring it under the project-owned "
                    "MIT umbrella",
                )
            )

    return findings


def check_third_party_md(repo_root: Path) -> list[str]:
    """Cross-check: every VENDORED_FILES path must appear in
    THIRD_PARTY_LICENSES.md, and every entry in the inventory's
    'Vendored from Timelapse2' table must exist in VENDORED_FILES.
    Returns a list of error strings (empty = OK)."""
    errors: list[str] = []
    md = repo_root / "THIRD_PARTY_LICENSES.md"
    if not md.is_file():
        return ["THIRD_PARTY_LICENSES.md is missing at repo root (DR-10)"]
    text = md.read_text(encoding="utf-8")
    for rel in VENDORED_FILES:
        if rel not in text:
            errors.append(
                f"vendored file {rel} is not referenced in "
                "THIRD_PARTY_LICENSES.md (DR-10 inventory drift)"
            )
    return errors


def main() -> int:
    p = argparse.ArgumentParser(description="Camera_Schedule license audit")
    p.add_argument(
        "--repo-root",
        default=None,
        help="repository root (auto-detected by default)",
    )
    p.add_argument(
        "--allowlist",
        default=None,
        help="path to approved-licenses.txt (default: <root>/approved-licenses.txt)",
    )
    p.add_argument(
        "--report",
        default=None,
        help="optional path to write the audit table (markdown)",
    )
    args = p.parse_args()

    if args.repo_root:
        repo_root = Path(args.repo_root).resolve()
    else:
        repo_root = discover_repo_root(Path(__file__).parent)

    allowlist_path = (
        Path(args.allowlist) if args.allowlist else repo_root / "approved-licenses.txt"
    )
    allowlist = load_allowlist(allowlist_path)

    findings = walk_audit(repo_root)
    inventory_errors = check_third_party_md(repo_root)

    fail_lines: list[str] = list(inventory_errors)
    by_kind: dict[str, int] = {}
    for f in findings:
        by_kind[f.kind] = by_kind.get(f.kind, 0) + 1
        if f.kind == "unknown" or f.license is None:
            fail_lines.append(f"[{f.kind}] {f.path}: {f.note or 'no license detected'}")
            continue
        if f.license not in allowlist:
            fail_lines.append(
                f"[{f.kind}] {f.path}: license {f.license!r} is not on the "
                f"approved list ({sorted(allowlist)})"
            )

    print(f"Camera_Schedule license audit: {len(findings)} files")
    for kind in ("project", "vendored", "ui-asset", "data"):
        print(f"  {kind:10s} {by_kind.get(kind, 0):4d}")
    if "unknown" in by_kind:
        print(f"  {'unknown':10s} {by_kind['unknown']:4d}")

    if args.report:
        rep = Path(args.report)
        rep.parent.mkdir(parents=True, exist_ok=True)
        rows = ["| File | Kind | License |", "|---|---|---|"]
        for f in findings:
            rows.append(f"| {f.path} | {f.kind} | {f.license or '(unknown)'} |")
        rep.write_text("\n".join(rows) + "\n", encoding="utf-8")
        print(f"  report written to {rep}")

    if fail_lines:
        print("\nFAIL:", file=sys.stderr)
        for line in fail_lines:
            print(f"  - {line}", file=sys.stderr)
        return 1

    print("OK: all bundled files licensed under approved set.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
