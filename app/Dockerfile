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
# BR-6 (reproducibility) — see DL-26.
#
# The integrator fills SDK_DIGEST at tag time by running:
#
#   docker buildx imagetools inspect \
#     axisecp/acap-native-sdk:${SDK_VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION} \
#     | awk '/^Digest:/ {print $2}'
#
# If SDK_DIGEST is non-empty (e.g. `--build-arg SDK_DIGEST=sha256:abc...`),
# the FROM line below uses the immutable @sha256: form. If unset
# (default for dev builds), it falls back to the tag — which is stable
# inside a single CI run because both reproducibility runs hit the
# same already-pulled local image. SOURCE_DATE_EPOCH and the post-build
# repack (app/scripts/repack_eap.sh) handle the remaining determinism
# plumbing on the .eap container itself.

ARG SDK_VERSION=12.6.0
ARG UBUNTU_VERSION=24.04
ARG ARCH=armv7hf
ARG SDK_DIGEST=

FROM axisecp/acap-native-sdk:${SDK_VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}${SDK_DIGEST:+@${SDK_DIGEST}} AS builder

ARG ARCH
ENV ARCH=${ARCH}

WORKDIR /opt/app

# Copy the application sources. The .dockerignore filters out build
# artifacts and host-test outputs.
COPY . /opt/app/

# Cache-bust the static front-end assets (DL-28).
#
# AXIS serves html/, js/, css/ via the system web server with only
# ETag/Last-Modified and NO Cache-Control, so browsers heuristically
# cache JS/CSS across app updates and serve stale code (observed on the
# OS 11 lab camera after a hot-fix redeploy). We can't set response
# headers on system-served static files, so we version the sub-resource
# URLs instead: append ?v=<token> to every js/css <script>/<link> ref.
# When asset content changes the URL changes, forcing a fresh fetch.
#
# The token is a 12-char SHA-256 over the JS+CSS *content* (not the app
# version): two builds at the same version with different asset content
# must get different tokens, or a hot-fix ships stale. Deterministic —
# same content => same token => byte-identical html => reproducible
# (BR-6). HTML is left to the browser's prompt top-level revalidation;
# only sub-resources are versioned.
RUN ASSET_VER=$(find html/js html/css -type f \( -name '*.js' -o -name '*.css' \) \
        | LC_ALL=C sort | xargs cat | sha256sum | cut -c1-12) \
    && sed -i -E \
        "s#href=\"(css/[^\"]+\.css)\"#href=\"\1?v=${ASSET_VER}\"#g; \
         s#src=\"(js/[^\"]+\.js)\"#src=\"\1?v=${ASSET_VER}\"#g" \
        html/*.html \
    && echo "asset cache-bust token: ${ASSET_VER}"

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
