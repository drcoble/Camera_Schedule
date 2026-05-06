#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Camera_Schedule contributors
#
# soak_24h.sh — 24-hour full soak harness for Camera_Schedule.
#
# This is the post-tag gating step for M7. Run after v0.7.0 is tagged.
# The tag is allowed to ship before this completes; M7 is "fully done"
# only when this report is clean and committed.
#
# Required environment variables:
#   AXIS_HOST_OS12  — IP of the OS 12 lab camera (see project memory file)
#   AXIS_PASS       — root password (see project memory file; do NOT commit)
#
# Usage:
#   export AXIS_HOST_OS12=10.1.40.113
#   export AXIS_PASS='...'
#   ./soak_24h.sh
#
# Consider running in a screen/tmux session — it takes 24 hours.

WINDOW=86400
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=soak_common.sh
. "${SCRIPT_DIR}/soak_common.sh"
soak_run
