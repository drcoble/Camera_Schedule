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
        src/acap/ACAP.c \
        src/acap/cJSON.c

# Pull in the same library set Timelapse2 uses; the AXEvent + AXParameter
# + VAPIX (libcurl) + FastCGI plumbing in ACAP.c needs all of these.
PKGS  = glib-2.0 gio-2.0 axevent axparameter fcgi libcurl

CFLAGS  += -std=c99 -Wall -Wextra -Wno-format-truncation -Wno-format-overflow
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

.PHONY: build-armv7hf build-aarch64 build-all dist-clean help

build-armv7hf: ## Build the armv7hf .eap (OS 11.11+ and OS 12 on Artpec-7)
	@mkdir -p $(DIST_DIR)
	docker build \
		--build-arg ARCH=armv7hf \
		--build-arg SDK_VERSION=$(SDK_VERSION) \
		--build-arg UBUNTU_VERSION=$(UBUNTU_VERSION) \
		--target export --output type=local,dest=$(DIST_DIR)/armv7hf \
		$(APP_DIR)
	@cp -f $(DIST_DIR)/armv7hf/*.eap $(DIST_DIR)/camera-schedule-armv7hf.eap
	@ls -la $(DIST_DIR)/camera-schedule-armv7hf.eap

build-aarch64: ## Build the aarch64 .eap (OS 12 on Artpec-8+)
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
