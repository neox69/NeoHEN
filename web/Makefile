# PS5AIO payload
#
#   make                  build ps5aio.elf for the PS5 (needs PS5_PAYLOAD_SDK)
#   make host             build build/ps5aio-host, a Linux build that emulates
#                         the console below $PS5AIO_ROOT (for development/tests)
#   make test             send ps5aio.elf to the console's ELF loader
#
# SPDX-License-Identifier: GPL-3.0-or-later

PS5_HOST ?= ps5
PS5_PORT ?= 9021

ELF      := neoweb.elf
HOST_BIN := build/neoweb-host

COMMON_SRCS := src/main.c src/http.c src/json.c src/log.c src/jobs.c \
               src/fsutil.c src/sha256.c src/sfo.c src/api.c src/api_fs.c src/api_apps.c \
               src/api_payloads.c src/api_homebrew.c src/webui.c src/plat_common.c
# The default build has every route: file manager, memory tools, dumper,
# installer and power. SAFE=1 builds the restricted variant, which only reads
# state, launches or closes games and manages NeoHEN's own payloads.
ifndef SAFE
COMMON_SRCS += src/api_mem.c src/api_dump.c src/api_saves.c src/api_captures.c src/api_stats.c src/api_trophies.c src/api_games.c src/api_cheats.c src/api_smp.c src/api_store.c src/api_launcher.c src/api_usb.c src/api_neohen.c src/api_unpack.c src/api_cleanup.c src/sqlite_ro.c src/zipstream.c src/discovery.c
# NeoHEN's own update (System > NeoHEN update)
COMMON_SRCS += src/api_update.c
FULLDEF     := -DNEOWEB_FULL
else
# the handlers of the routes that are compiled out stay in the sources
FULLDEF     := -Wno-unused-function
endif
HEADERS     := $(wildcard src/*.h)
# miniz (ZIP and deflate, for /api/fs/extract) is third-party code: it is
# built on its own, without -Werror
MINIZ       := ../third_party/cheat_support/miniz.c
WEBUI       := webui/index.html
LAUNCHER    := launcher/icon0.png launcher/pic1.png
JXRJS       := jxr/jxr.js

WARN   := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation -Werror
# The build variant (SAFE=1 or not) is kept in a stamp file: the binaries
# depend on it, so switching variants rebuilds them instead of keeping the
# other variant's binary. The stamp is only rewritten when the variant changes.
VARIANT  := $(if $(SAFE),safe,full)
STAMP    := build/variant-$(VARIANT).stamp
$(shell mkdir -p build; [ -f $(STAMP) ] || { rm -f build/variant-*.stamp; touch $(STAMP); })

# The NeoHEN release this NeoWEB ships with, from the repo's VERSION file
# (api.h has a fallback for builds outside the repo). VERSION is listed with
# the headers so a new release number rebuilds the binaries.
NEOHEN_VERSION := $(shell tr -d ' \t\r\n' < ../VERSION 2>/dev/null)
HEADERS += $(wildcard ../VERSION)

DEFS   := $(if $(NEOHEN_VERSION),-DNEOHEN_VERSION=\"$(NEOHEN_VERSION)\") -DLAUNCHER_DIR=\"$(CURDIR)/launcher\" -DWEBUI_PATH=\"$(CURDIR)/$(WEBUI)\" -DJXRJS_PATH=\"$(CURDIR)/$(JXRJS)\" $(FULLDEF)

# The page and jxr.js go into the binary gzipped: webui.c hands them as they
# are to browsers (all of them take gzip) and inflates them for the rest. Their
# ETags are the hash of the files, worked out here (see the rules at the end).
WEBUI_GZ    := build/index.html.gz
JXRJS_GZ    := build/jxr.js.gz
WEBUI_ETAGS := build/webui_etag.h
WEBUI_EMBED := $(WEBUI_GZ) $(JXRJS_GZ) $(WEBUI_ETAGS)
SHA256      := $(shell command -v sha256sum >/dev/null 2>&1 && echo sha256sum || echo 'shasum -a 256')
DEFS   += -Ibuild -DWEBUI_GZ_PATH=\"$(CURDIR)/$(WEBUI_GZ)\" -DJXRJS_GZ_PATH=\"$(CURDIR)/$(JXRJS_GZ)\"

.PHONY: all host clean test

ifeq ($(MAKECMDGOALS),host)
all: host
else ifeq ($(MAKECMDGOALS),clean)
all:
else
ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
$(error PS5_PAYLOAD_SDK is undefined (export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk) - or use 'make host')
endif

all: $(ELF)

build/miniz-ps5.o: $(MINIZ)
	@mkdir -p build
	$(CC) -O2 -g -w -c -o $@ $<

$(ELF): $(COMMON_SRCS) src/plat_ps5.c $(HEADERS) $(WEBUI) $(JXRJS) $(LAUNCHER) build/miniz-ps5.o $(STAMP)
	$(CC) -O2 -g $(WARN) -Isrc $(DEFS) -o $@ $(COMMON_SRCS) src/plat_ps5.c build/miniz-ps5.o \
	  -lkernel_sys -lSceSystemService -lSceUserService -lSceAppInstUtil

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^
endif

HOST_CC ?= cc

host: $(HOST_BIN)

build/miniz-host.o: $(MINIZ)
	@mkdir -p build
	$(HOST_CC) -O1 -g -w -c -o $@ $<

$(HOST_BIN): $(COMMON_SRCS) src/plat_host.c $(HEADERS) $(WEBUI) $(JXRJS) $(LAUNCHER) build/miniz-host.o $(STAMP)
	@mkdir -p build
	$(HOST_CC) -O1 -g $(WARN) -D_GNU_SOURCE -Isrc $(DEFS) -o $@ \
	  $(COMMON_SRCS) src/plat_host.c build/miniz-host.o -lpthread -lm

clean:
	rm -rf $(ELF) build

# ------------------------------------------------------------ embedded page
$(HOST_BIN) $(ELF): $(WEBUI_EMBED)

$(WEBUI_GZ): $(WEBUI)
	@mkdir -p build
	gzip -9 -n -c $< > $@.tmp && mv $@.tmp $@

$(JXRJS_GZ): $(JXRJS)
	@mkdir -p build
	gzip -9 -n -c $< > $@.tmp && mv $@.tmp $@

$(WEBUI_ETAGS): $(WEBUI) $(JXRJS)
	@mkdir -p build
	{ printf '#define WEBUI_ETAG "%s"\n' "$$($(SHA256) < $(WEBUI) | cut -c1-24)"; \
	  printf '#define JXRJS_ETAG "%s"\n' "$$($(SHA256) < $(JXRJS) | cut -c1-24)"; } > $@.tmp && mv $@.tmp $@
