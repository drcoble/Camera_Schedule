# syntax=docker/dockerfile:1.6
#
# Camera_Schedule build container.
#
# Single Dockerfile, parameterized by ARCH. Two artifacts come out of
# this image: camera-schedule-armv7hf.eap and camera-schedule-aarch64.eap.
#
# Build:
#   docker build --build-arg ARCH=armv7hf -t cs-build-armv7hf .
#   docker build --build-arg ARCH=aarch64 -t cs-build-aarch64 .
#
# The Makefile drives both via `make build-all`.
#
# BR-6 (reproducibility) wants a digest pin instead of a tag, but the
# digest changes per SDK release. The CI workflow records the resolved
# digest on every build; before tagging v1.0 (M8), the production tags
# below should be replaced with @sha256:... pins.

ARG SDK_VERSION=12.6.0
ARG UBUNTU_VERSION=24.04
ARG ARCH=armv7hf

FROM axisecp/acap-native-sdk:${SDK_VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION} AS builder

ARG ARCH
ENV ARCH=${ARCH}

WORKDIR /opt/app

# Copy the application sources. The .dockerignore filters out build
# artifacts and host-test outputs.
COPY . /opt/app/

# Source the SDK environment (architecture-specific) and build the
# .eap. acap-build emits camera_schedule_<version>_<arch>.eap; the
# Makefile renames it to camera-schedule-<arch>.eap.
RUN . /opt/axis/acapsdk/environment-setup-* \
    && acap-build . -a settings/events.json
# acap-build only auto-bundles a fixed set of well-known directories
# (html/, lib/, etc.). Arbitrary files like settings/events.json must
# be passed via -a or they'll be silently dropped from the .eap. The
# event-topic registration path (ACAP_FILE_Read("settings/events.json")
# in vendored ACAP.c at line 1613) reads this file at boot to declare
# AXEvent topics — without it, no topics appear in the camera's Action
# Rules picker. Add a `-a` for any new file in app/ that isn't under
# html/ but needs to ship with the .eap.

# Final stage: a tiny image whose only job is to expose the .eap on
# `docker create + docker cp`.
FROM scratch AS export
COPY --from=builder /opt/app/*.eap /
