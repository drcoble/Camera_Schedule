#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Camera_Schedule contributors
#
# Extract the section of CHANGELOG.md matching a given tag and write
# it to a release-notes file. Used by .github/workflows/release.yml
# to populate the GitHub Release body.
#
# Convention: Keep a Changelog (https://keepachangelog.com).
# Section headers are `## [vX.Y.Z] - DATE` or `## vX.Y.Z` or
# `## [X.Y.Z]` — all three are matched.
#
# If no matching section is found, a placeholder body is written so
# the release workflow does not fail. The integrator can amend the
# draft release before publishing.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def extract(changelog: str, tag: str) -> str:
    stripped = tag.lstrip("v")
    patterns = [
        re.compile(rf"^##\s+\[?v?{re.escape(stripped)}\]?.*$"),
        re.compile(rf"^##\s+\[?{re.escape(tag)}\]?.*$"),
    ]
    lines = changelog.splitlines()
    start = None
    for i, line in enumerate(lines):
        if any(p.match(line) for p in patterns):
            start = i
            break
    if start is None:
        return (
            f"Release {tag} of Camera_Schedule.\n\n"
            "CHANGELOG.md did not contain a matching section for this "
            "tag at workflow run time. Edit the draft release body "
            "before publishing, or re-run after the section lands.\n"
        )
    end = len(lines)
    for j in range(start + 1, len(lines)):
        if lines[j].startswith("## "):
            end = j
            break
    return "\n".join(lines[start:end]).strip() + "\n"


def main() -> int:
    p = argparse.ArgumentParser(description="Extract release notes from CHANGELOG.md")
    p.add_argument("--tag", required=True, help="release tag (e.g. v1.0.0-beta)")
    p.add_argument(
        "--changelog",
        default="CHANGELOG.md",
        help="path to CHANGELOG.md (default: ./CHANGELOG.md)",
    )
    p.add_argument(
        "--output",
        required=True,
        help="path to write the release-notes file",
    )
    args = p.parse_args()

    cl_path = Path(args.changelog)
    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    if cl_path.is_file():
        text = cl_path.read_text(encoding="utf-8")
        body = extract(text, args.tag)
    else:
        body = (
            f"Release {args.tag} of Camera_Schedule.\n\n"
            "CHANGELOG.md was not present at tag time.\n"
        )

    out_path.write_text(body, encoding="utf-8")
    print(f"Wrote {out_path} ({len(body)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
