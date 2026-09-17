include ../../Rules.mk

# --------------------------------------------------------------------------
# jq (JSON processor)
#
# The device-parameter feature (see modules/tedge/merge/bin/parameter-update and
# parameter-plugins/) parses the JSON payload of the Cumulocity
# c8y_ParameterUpdate operation. ICR-OS busybox ships no JSON tooling at all --
# no jq, no python -- and hand-rolling a JSON parser in awk for cloud-supplied,
# nested input would be both fragile and a security liability. jq publishes
# statically linked, dependency-free binaries per CPU architecture at:
#   https://github.com/jqlang/jq/releases
#
# This package lays the matching binary into the tedge platform tree at
# bin/jq (which is on the PATH of tedge-agent and of every module script via
# /opt/tedge/env), so the payload unpacks straight into /opt/tedge.
# --------------------------------------------------------------------------

# Pinned jq release (see version.txt / CHANGELOG).
JQ_VERSION := 1.8.2
PKGNAME    := jq-$(JQ_VERSION)

# Map the ICR build PLATFORM onto the jq release architecture. These match the
# thin-edge.io standalone architectures used by packages/tedge.
#   v2i  = armv5 soft-float EABI      -> armel
#   v3   = armv7 EABI                 -> armhf
#   v4   = aarch64                    -> arm64
#   v4i  = aarch64  (ICR-1642)        -> arm64
#   pc   = x86_64  (host smoke test)  -> amd64
ifeq ($(PLATFORM),v2i)
JQ_ARCH := armel
else ifeq ($(PLATFORM),v3)
JQ_ARCH := armhf
else ifneq ($(filter $(PLATFORM),v4 v4i),)
JQ_ARCH := arm64
else ifeq ($(PLATFORM),pc)
JQ_ARCH := amd64
else
JQ_ARCH := unsupported
endif

BINARY   := jq-linux-$(JQ_ARCH)
BASE_URL := https://github.com/jqlang/jq/releases/download/jq-$(JQ_VERSION)
DESTDIR  := $(CURDIR)/tmp

# Host tools required to assemble the package. build-package's `check` target
# warns (and skips) gracefully if any are missing.
CMD_LIST := curl tar gzip

$(eval $(build-package))

$(PKGNAME).$(PLATFORM).pkg:
	@if [ "$(JQ_ARCH)" = "unsupported" ]; then \
		echo "  WARNING: no jq build for PLATFORM=$(PLATFORM)"; \
		touch $@; exit 0; \
	fi
	@rm -rf $(DESTDIR) *.$(PLATFORM).pkg
	@mkdir -p $(DESTDIR)/bin
	@echo "  Fetching $(BINARY)"
	@if [ ! -f "$(BINARY)" ]; then \
		curl -fSL --retry 3 -o "$(BINARY)" "$(BASE_URL)/$(BINARY)"; \
	fi
	@echo "  Installing jq $(JQ_VERSION) ($(JQ_ARCH)) for $(PLATFORM)"
	@cp "$(BINARY)" $(DESTDIR)/bin/jq
	@chmod +x $(DESTDIR)/bin/jq
	@install -d $(DESTDIR)/license
	@printf 'jq %s (%s)\nSPDX-License-Identifier: MIT\nSee https://github.com/jqlang/jq\n' \
		"$(JQ_VERSION)" "$(JQ_ARCH)" > $(DESTDIR)/license/$(PKGNAME).txt
	@echo "  Creating $@"
	@# Payload is the *contents* of DESTDIR (bin/, license/); build-module
	@# unpacks it straight into tmp/opt/tedge.
	@tar -czf $@ -C $(DESTDIR) .
	@rm -rf $(DESTDIR)

clean:
	@rm -rf $(DESTDIR) *.pkg $(BINARY)
