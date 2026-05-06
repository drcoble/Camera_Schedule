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
        src/astro/solar.c \
        src/astro/lunar.c \
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

.PHONY: build-armv7hf build-aarch64 build-all dist-clean help stage-license test test-lunar test-clean

# ---------------------------------------------------------------------
# Host-side tests (no docker, no SDK)
# ---------------------------------------------------------------------

# Solar and lunar tests run natively on the build host. Both modules
# are pure (no GLib, no ACAP framework) and link against only libm.
TEST_HOST_DIR   := $(APP_DIR)/test/host
TEST_HOST_BIN   := $(TEST_HOST_DIR)/build/test_solar
TEST_LUNAR_BIN  := $(TEST_HOST_DIR)/build/test_lunar
# C11 for test_lunar.c (uses compound-literal reset idiom); solar only
# needs C99. Use c11 for both to keep a single CFLAGS set.
TEST_CFLAGS     := -std=c11 -Wall -Wextra -Wpedantic -O2 -I$(APP_DIR)/src

test: $(TEST_HOST_BIN) ## Run host-side test fixtures (solar + lunar)
	@$(TEST_HOST_BIN)
	@if [ -f $(APP_DIR)/src/astro/lunar.c ]; then \
	    $(MAKE) --no-print-directory test-lunar; \
	else \
	    echo "test_lunar: SKIPPED (lunar.c not yet available from SSE-1)"; \
	fi

# Separate target so `make test` keeps running the solar suite even
# while lunar.c is not yet available from SSE-1. Once lunar.c lands,
# both suites run and a non-zero exit from test_lunar propagates.
test-lunar: $(TEST_LUNAR_BIN) ## Build and run the lunar test suite (requires lunar.c from SSE-1)
	@$(TEST_LUNAR_BIN)

$(TEST_HOST_BIN): $(APP_DIR)/src/astro/solar.c \
                  $(APP_DIR)/src/astro/solar.h \
                  $(TEST_HOST_DIR)/test_solar.c
	@mkdir -p $(TEST_HOST_DIR)/build
	$(CC) $(TEST_CFLAGS) \
	  $(APP_DIR)/src/astro/solar.c \
	  $(TEST_HOST_DIR)/test_solar.c \
	  -lm -o $(TEST_HOST_BIN)

# lunar.c will be provided by SSE-1 (M4 implementation). Until then
# this target fails at link time with a clear "undefined reference"
# message. test_lunar.c compiles standalone:
#   cc -std=c11 -Wall -Wextra -Wpedantic -O2 -I app/src -c \
#      app/test/host/test_lunar.c
$(TEST_LUNAR_BIN): $(APP_DIR)/src/astro/lunar.c \
                   $(APP_DIR)/src/astro/lunar.h \
                   $(TEST_HOST_DIR)/test_lunar.c
	@mkdir -p $(TEST_HOST_DIR)/build
	$(CC) $(TEST_CFLAGS) \
	  $(APP_DIR)/src/astro/lunar.c \
	  $(TEST_HOST_DIR)/test_lunar.c \
	  -lm -o $(TEST_LUNAR_BIN)

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
		--target export --output type=local,dest=$(DIST_DIR)/armv7hf \
		$(APP_DIR)
	@cp -f $(DIST_DIR)/armv7hf/*.eap $(DIST_DIR)/camera-schedule-armv7hf.eap
	@ls -la $(DIST_DIR)/camera-schedule-armv7hf.eap

build-aarch64: stage-license ## Build the aarch64 .eap (OS 12 on Artpec-8+)
	@mkdir -p $(DIST_DIR)
	docker build \
		--build-arg ARCH=aarch64 \
		--build-arg SDK_VERSION=$(SDK_VERSION) \
		--build-arg UBUNTU_VERSION=$(UBUNTU_VERSION) \
		--target export --output type=local,dest=$(DIST_DIR)/aarch64 \
		$(APP_DIR)
	@cp -f $(DIST_DIR)/aarch64/*.eap $(DIST_DIR)/camera-schedule-aarch64.eap
	@ls -la $(DIST_DIR)/camera-schedule-aarch64.eap

build-all: build-armv7hf build-aarch64 ## Build both release artifacts

dist-clean: ## Remove dist/ output
	rm -rf $(DIST_DIR)

help: ## Show this help
	@awk 'BEGIN{FS=":.*?## "} /^[a-zA-Z0-9_-]+:.*?## / {printf "  %-20s %s\n", $$1, $$2}' $(MAKEFILE_LIST) | sort
