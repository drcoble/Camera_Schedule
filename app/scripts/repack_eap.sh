#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Camera_Schedule contributors
#
# Repack a .eap file deterministically (BR-6).
#
# `acap-build` produces a gzip'd tar (the .eap) whose content is
# bit-stable but whose container is not: tar entries can come out in
# filesystem-order, file mtimes reflect build-time, and gzip embeds
# its own timestamp + filename. Two clean checkouts of the same source
# therefore produce two .eap files with the same payload but different
# SHA-256.
#
# This script normalizes the container:
#   * decompress
#   * tar --sort=name + --mtime=@$SOURCE_DATE_EPOCH + --owner=0
#     --group=0 --numeric-owner
#   * gzip -n (no name, no mtime)
#
# Inputs:
#   $1 — path to the .eap to rewrite in place
#   SOURCE_DATE_EPOCH (env) — UNIX seconds, required.
#
# Notes:
#   * GNU tar 1.28+ is required (the `--sort=name` flag landed there).
#   * `gzip -n` strips the original filename + mtime fields per RFC
#     1952 §2.3.1; combined with `-9` we get a deterministic gzip
#     stream for a deterministic input.
#   * We rebuild via `tar -tf | sort | tar -T -` rather than
#     `--sort=name` directly to keep this portable to BSD tar (the
#     macOS host case for local-dev determinism). GNU tar's
#     --sort=name is preferred on CI; the fallback path is the same
#     SHA when both ends use it.

set -euo pipefail

EAP="${1:?usage: repack_eap.sh <eap-file>}"
: "${SOURCE_DATE_EPOCH:?SOURCE_DATE_EPOCH must be set (BR-6 / DL-25)}"

if [[ ! -f "$EAP" ]]; then
  echo "repack_eap: file not found: $EAP" >&2
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Stage 1: extract.
mkdir -p "$WORK/payload"
gunzip -c "$EAP" | tar -xf - -C "$WORK/payload"

# Stage 2: normalize file timestamps. SOURCE_DATE_EPOCH is in UNIX
# seconds; touch's `-d @sec` form is the portable way to set both
# mtime and atime to that value.
find "$WORK/payload" -exec touch -h -d "@${SOURCE_DATE_EPOCH}" {} +

# Stage 3: re-tar with sorted entries, normalized owner/group/mtime.
# GNU tar honors --sort; if we're on a host without it, fall back to
# generating a sorted file list and feeding it via -T.
TAR_FLAGS=(
  --owner=0
  --group=0
  --numeric-owner
  --mtime="@${SOURCE_DATE_EPOCH}"
  --format=ustar
)

if tar --help 2>&1 | grep -qE -- '--sort='; then
  (cd "$WORK/payload" && tar "${TAR_FLAGS[@]}" --sort=name -cf "$WORK/repacked.tar" .)
else
  (cd "$WORK/payload" \
     && find . -print0 \
     | LC_ALL=C sort -z \
     | tar "${TAR_FLAGS[@]}" --null -T - -cf "$WORK/repacked.tar")
fi

# Stage 4: gzip -n9 for deterministic compression. -n drops the
# filename/timestamp metadata fields from the gzip header.
gzip -n9 < "$WORK/repacked.tar" > "$WORK/repacked.eap"

# Stage 5: replace in place.
mv "$WORK/repacked.eap" "$EAP"

echo "repack_eap: $EAP normalized (SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH})"
