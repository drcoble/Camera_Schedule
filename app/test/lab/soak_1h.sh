#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Camera_Schedule contributors
#
# soak_1h.sh — 1-hour pilot soak harness for Camera_Schedule.
#
# Run this before tagging v0.7.0. If this passes, it's safe to tag;
# the 24-hour run (soak_24h.sh) is a post-tag gating step.
#
# Required environment variables:
#   AXIS_HOST_OS12  — IP of the OS 12 lab camera (see project memory file)
#   AXIS_PASS       — root password (see project memory file; do NOT commit)
#
# Usage:
#   export AXIS_HOST_OS12=10.1.40.113
#   export AXIS_PASS='...'
#   ./soak_1h.sh

WINDOW=3600
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=soak_common.sh
. "${SCRIPT_DIR}/soak_common.sh"
soak_run
