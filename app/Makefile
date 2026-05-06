# Camera_Schedule application Makefile.
#
# Two roles:
#   1. In-container: invoked by acap-build inside the SDK image. Compiles
#      the C sources to a single ELF named after the appName in
#      manifest.json (`camera_schedule`), which acap-build then bundles
#      into a .eap file. The PROG/OBJS/PKGS variables drive that path.
#   2. On the host (outside Docker): `make build-armv7hf`,
#      `make build-aarch64`, and `make build-all` shell out to docker
#      to produce the two release artifacts.
#
# Run `make help` for the menu.

# ---------------------------------------------------------------------
# In-container build (driven by acap-build)
# ---------------------------------------------------------------------

PROG  = camera_schedule
OBJS  = src/main.c \
        src/timers.c \
        src/anchors.c \
        src/calendar.c \
        src/persistence.c \
        src/status.c \
        src/astro/solar.c \
        src/astro/lunar.c \
        src/astro/seasonal.c \
        src/acap/ACAP.c \
        src/acap/cJSON.c

# Pull in the same library set Timelapse2 uses; the AXEvent + AXParameter
# + VAPIX (libcurl) + FastCGI plumbing in ACAP.c needs all of these.
PKGS  = glib-2.0 gio-2.0 axevent axparameter fcgi libcurl

CFLAGS  += -std=c99 -Wall -Wextra -Wno-format-truncation -Wno-format-overflow
# _GNU_SOURCE so strdup/strndup are declared by string.h (vendored
# ACAP.c uses both). Without it, gcc treats them as implicit-int and
# silently truncates pointers on 64-bit targets.
CFLAGS  += -D_GNU_SOURCE
CFLAGS  += -Isrc -Isrc/acap
CFLAGS  += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))

LDLIBS  += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))
LDLIBS  += -s -lm -ldl -lpthread

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

# `acap-build .` invokes the default target above, then bundles the
# resulting ELF + manifest.json + html/ + settings/ into a .eap.

.PHONY: clean
clean:
	rm -f $(PROG) src/*.o src/acap/*.o src/astro/*.o
	rm -f *.eap *.eap.old package.conf* param.conf *_LICENSE.txt manifest.json.bak

# ---------------------------------------------------------------------
# Host-side targets (call docker to build the two release .eap files)
# ---------------------------------------------------------------------

# Repo root is one above this Makefile's directory.
REPO_ROOT       := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
APP_DIR         := $(REPO_ROOT)/app
DIST_DIR        := $(REPO_ROOT)/dist
SDK_VERSION     ?= 12.6.0
UBUNTU_VERSION  ?= 24.04

# Optional SDK image digest pin (BR-6 / DL-25). Empty by default —
# release.yml resolves the digest at tag time and exports
# SDK_DIGEST_<arch> in the env. Per-arch because the SDK image is
# multi-platform and each arch has its own digest.
SDK_DIGEST_armv7hf ?=
SDK_DIGEST_aarch64 ?=

.PHONY: build-armv7hf build-aarch64 build-all dist-clean help stage-license \
        test test-lunar test-seasonal test-anchors test-calendar \
        test-export-import test-clean license-audit reproducibility-check

# ---------------------------------------------------------------------
# Reproducibility plumbing (BR-6 / DL-25)
# ---------------------------------------------------------------------
#
# SOURCE_DATE_EPOCH pins the timestamp embedded in tar entries and the
# repacked .eap. The default below derives it from the head commit's
# author timestamp so a clean checkout at the same SHA produces the
# same value. CI overrides via env (`SOURCE_DATE_EPOCH=$(git log -1
# --pretty=%ct)` is the canonical form).
#
# If git isn't available (release-tarball builds), the env var must
# be supplied by the caller.
SOURCE_DATE_EPOCH ?= $(shell git -C $(REPO_ROOT) log -1 --pretty=%ct 2>/dev/null)
export SOURCE_DATE_EPOCH

# LC_ALL=C pins sort/locale-sensitive utilities to a stable order.
export LC_ALL := C

REPACK_EAP := bash $(APP_DIR)/scripts/repack_eap.sh

# ---------------------------------------------------------------------
# Host-side tests (no docker, no SDK)
# ---------------------------------------------------------------------

# Solar, lunar, and seasonal tests run natively on the build host. All
# three modules are pure (no GLib, no ACAP framework) and link against
# only libm.
TEST_HOST_DIR      := $(APP_DIR)/test/host
TEST_HOST_BIN      := $(TEST_HOST_DIR)/build/test_solar
TEST_LUNAR_BIN     := $(TEST_HOST_DIR)/build/test_lunar
TEST_SEASONAL_BIN  := $(TEST_HOST_DIR)/build/test_seasonal
TEST_ANCHORS_BIN   := $(TEST_HOST_DIR)/build/test_anchors
TEST_CALENDAR_BIN  := $(TEST_HOST_DIR)/build/test_calendar
TEST_EXPORT_BIN    := $(TEST_HOST_DIR)/build/test_export_import
# C11 for test_lunar.c (uses compound-literal reset idiom); solar only
# needs C99. Use c11 for the whole set to keep a single CFLAGS line.
TEST_CFLAGS        := -std=c11 -Wall -Wextra -Wpedantic -O2 -I$(APP_DIR)/src

test: $(TEST_HOST_BIN) $(TEST_LUNAR_BIN) $(TEST_SEASONAL_BIN) $(TEST_ANCHORS_BIN) $(TEST_CALENDAR_BIN) $(TEST_EXPORT_BIN) ## Run host-side test fixtures (solar + lunar + seasonal + anchors + calendar + export/import)
	@$(TEST_HOST_BIN)
	@$(TEST_LUNAR_BIN)
	@$(TEST_SEASONAL_BIN)
	@$(TEST_ANCHORS_BIN)
	@$(TEST_CALENDAR_BIN)
	@$(TEST_EXPORT_BIN)

test-lunar: $(TEST_LUNAR_BIN) ## Build and run the lunar test suite
	@$(TEST_LUNAR_BIN)

test-seasonal: $(TEST_SEASONAL_BIN) ## Build and run the seasonal test suite
	@$(TEST_SEASONAL_BIN)

test-anchors: $(TEST_ANCHORS_BIN) ## Build and run the anchors test suite
	@$(TEST_ANCHORS_BIN)

test-calendar: $(TEST_CALENDAR_BIN) ## Build and run the calendar test suite
	@$(TEST_CALENDAR_BIN)

test-export-import: $(TEST_EXPORT_BIN) ## Build and run the export/import round-trip suite (M7)
	@$(TEST_EXPORT_BIN)

$(TEST_HOST_BIN): $(APP_DIR)/src/astro/solar.c \
                  $(APP_DIR)/src/astro/solar.h \
                  $(TEST_HOST_DIR)/test_solar.c
	@mkdir -p $(TEST_HOST_DIR)/build
	$(CC) $(TEST_CFLAGS) \
	  $(APP_DIR)/src/astro/solar.c \
	  $(TEST_HOST_DIR)/test_solar.c \
	  -lm -o $(TEST_HOST_BIN)

$(TEST_LUNAR_BIN): $(APP_DIR)/src/astro/lunar.c \
                   $(APP_DIR)/src/astro/lunar.h \
                   $(TEST_HOST_DIR)/test_lunar.c
	@mkdir -p $(TEST_HOST_DIR)/build
	$(CC) $(TEST_CFLAGS) \
	  $(APP_DIR)/src/astro/lunar.c \
	  $(TEST_HOST_DIR)/test_lunar.c \
	  -lm -o $(TEST_LUNAR_BIN)

$(TEST_SEASONAL_BIN): $(APP_DIR)/src/astro/seasonal.c \
                      $(APP_DIR)/src/astro/seasonal.h \
                      $(TEST_HOST_DIR)/test_seasonal.c
	@mkdir -p $(TEST_HOST_DIR)/build
	$(CC) $(TEST_CFLAGS) \
	  $(APP_DIR)/src/astro/seasonal.c \
	  $(TEST_HOST_DIR)/test_seasonal.c \
	  -lm -o $(TEST_SEASONAL_BIN)

# Host-side anchor + calendar fixtures are pure C and link only against
# lunar.c (for lunar_illumination in test_anchors §2). They reference
# anchors.h / calendar.h for type and constant checks but make no calls
# into anchors.c / calendar.c — those are framework-aware modules that
# pull in GLib/AXEvent and don't compile on the build host. Integration-
# level assertions that need the real anchors.c / calendar.c are gated
# behind /* TODO at integration */ comments inside the fixtures.
$(TEST_ANCHORS_BIN): $(APP_DIR)/src/anchors.h \
                     $(APP_DIR)/src/calendar.h \
                     $(APP_DIR)/src/astro/lunar.h \
                     $(APP_DIR)/src/astro/lunar.c \
                     $(TEST_HOST_DIR)/test_anchors.c
	@mkdir -p $(TEST_HOST_DIR)/build
	$(CC) $(TEST_CFLAGS) \
	  $(APP_DIR)/src/astro/lunar.c \
	  $(TEST_HOST_DIR)/test_anchors.c \
	  -lm -o $(TEST_ANCHORS_BIN)

$(TEST_CALENDAR_BIN): $(APP_DIR)/src/calendar.h \
                      $(TEST_HOST_DIR)/test_calendar.c
	@mkdir -p $(TEST_HOST_DIR)/build
	$(CC) $(TEST_CFLAGS) \
	  $(TEST_HOST_DIR)/test_calendar.c \
	  -lm -o $(TEST_CALENDAR_BIN)

# M7 export/import host fixture. STE-owned — the SSE provides this build
# rule shell so the integrator wires the test into `make test` without
# Makefile churn. The fixture should exercise:
#   * envelope round-trip with the same JSON serializer the /export
#     handler uses (refactor build_export_envelope into something
#     callable from the host fixture, or duplicate the envelope-shape
#     code at the test boundary — STE's call).
#   * schema_mismatch, invalid_anchor, invalid_calendar,
#     invalid_schedule_enabled rejection paths from /import.
# Test source is `test/host/test_export_import.c`; missing-source
# behavior is handled by the make pattern via $(wildcard) so a clean
# `make test` succeeds even before STE lands the test.
TEST_EXPORT_SRC := $(wildcard $(TEST_HOST_DIR)/test_export_import.c)
$(TEST_EXPORT_BIN): $(APP_DIR)/src/anchors.h \
                    $(APP_DIR)/src/calendar.h
	@mkdir -p $(TEST_HOST_DIR)/build
	@if [ -n "$(TEST_EXPORT_SRC)" ]; then \
	  $(CC) $(TEST_CFLAGS) -I$(APP_DIR)/src/acap \
	    $(APP_DIR)/src/acap/cJSON.c \
	    $(TEST_EXPORT_SRC) \
	    -lm -o $(TEST_EXPORT_BIN) ; \
	else \
	  printf '#include <stdio.h>\nint main(void){puts("export/import test not yet provided by STE; skipping");return 0;}\n' \
	    > $(TEST_HOST_DIR)/build/_skip.c ; \
	  $(CC) -O0 $(TEST_HOST_DIR)/build/_skip.c -o $(TEST_EXPORT_BIN) ; \
	fi

test-clean: ## Remove host test build outputs
	rm -rf $(TEST_HOST_DIR)/build

# acap-build (the in-container packaging tool) requires a LICENSE file
# in the build directory. The canonical LICENSE lives at the repo root;
# this target copies it into app/ as a build artifact (gitignored) so
# the docker build context picks it up.
stage-license:
	@cp -f $(REPO_ROOT)/LICENSE $(APP_DIR)/LICENSE

build-armv7hf: stage-license ## Build the armv7hf .eap (OS 11.11+ and OS 12 on Artpec-7)
	@mkdir -p $(DIST_DIR)
	docker build \
		--build-arg ARCH=armv7hf \
		--build-arg SDK_VERSION=$(SDK_VERSION) \
		--build-arg UBUNTU_VERSION=$(UBUNTU_VERSION) \
		--build-arg SDK_DIGEST=$(SDK_DIGEST_armv7hf) \
		--target export --output type=local,dest=$(DIST_DIR)/armv7hf \
		$(APP_DIR)
	@cp -f $(DIST_DIR)/armv7hf/*.eap $(DIST_DIR)/camera-schedule-armv7hf.eap
	@$(REPACK_EAP) $(DIST_DIR)/camera-schedule-armv7hf.eap
	@ls -la $(DIST_DIR)/camera-schedule-armv7hf.eap

build-aarch64: stage-license ## Build the aarch64 .eap (OS 12 on Artpec-8+)
	@mkdir -p $(DIST_DIR)
	docker build \
		--build-arg ARCH=aarch64 \
		--build-arg SDK_VERSION=$(SDK_VERSION) \
		--build-arg UBUNTU_VERSION=$(UBUNTU_VERSION) \
		--build-arg SDK_DIGEST=$(SDK_DIGEST_aarch64) \
		--target export --output type=local,dest=$(DIST_DIR)/aarch64 \
		$(APP_DIR)
	@cp -f $(DIST_DIR)/aarch64/*.eap $(DIST_DIR)/camera-schedule-aarch64.eap
	@$(REPACK_EAP) $(DIST_DIR)/camera-schedule-aarch64.eap
	@ls -la $(DIST_DIR)/camera-schedule-aarch64.eap

# ---------------------------------------------------------------------
# License audit (NFR-6 / DR-11) — see DL-24.
# ---------------------------------------------------------------------
license-audit: ## Run the bundled-license audit (NFR-6 / DR-11)
	@python3 $(APP_DIR)/scripts/license_audit.py

# ---------------------------------------------------------------------
# Reproducibility check (BR-6) — build twice, assert SHA-256 identity.
# ---------------------------------------------------------------------
#
# This target is intended for local-dev verification; the full CI gate
# lives in `.github/workflows/reproducibility.yml` and runs the same
# logic on the GitHub Actions runner. Both architectures are checked
# in parallel makes; both must produce identical SHA-256 across the
# two runs.
reproducibility-check: ## Build each arch twice and assert SHA-256 identity (BR-6)
	@set -euo pipefail; \
	mkdir -p $(DIST_DIR)/_repro1 $(DIST_DIR)/_repro2; \
	for arch in armv7hf aarch64; do \
	  echo "==> reproducibility: $$arch run 1"; \
	  $(MAKE) -C $(APP_DIR) build-$$arch >/dev/null; \
	  cp $(DIST_DIR)/camera-schedule-$$arch.eap $(DIST_DIR)/_repro1/; \
	  echo "==> reproducibility: $$arch run 2 (clean)"; \
	  $(MAKE) -C $(APP_DIR) dist-clean >/dev/null; \
	  $(MAKE) -C $(APP_DIR) build-$$arch >/dev/null; \
	  cp $(DIST_DIR)/camera-schedule-$$arch.eap $(DIST_DIR)/_repro2/; \
	  H1=$$(sha256sum $(DIST_DIR)/_repro1/camera-schedule-$$arch.eap | awk '{print $$1}'); \
	  H2=$$(sha256sum $(DIST_DIR)/_repro2/camera-schedule-$$arch.eap | awk '{print $$1}'); \
	  echo "$$arch run1: $$H1"; \
	  echo "$$arch run2: $$H2"; \
	  if [ "$$H1" != "$$H2" ]; then \
	    echo "FAIL: $$arch .eap is not reproducible (BR-6)"; exit 1; \
	  fi; \
	  echo "PASS: $$arch reproducible"; \
	done; \
	rm -rf $(DIST_DIR)/_repro1 $(DIST_DIR)/_repro2

build-all: build-armv7hf build-aarch64 ## Build both release artifacts

dist-clean: ## Remove dist/ output
	rm -rf $(DIST_DIR)

help: ## Show this help
	@awk 'BEGIN{FS=":.*?## "} /^[a-zA-Z0-9_-]+:.*?## / {printf "  %-20s %s\n", $$1, $$2}' $(MAKEFILE_LIST) | sort
