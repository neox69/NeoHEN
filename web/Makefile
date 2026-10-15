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
               src/api_payloads.c src/webui.c src/plat_common.c
# The default build has every route: file manager, memory tools, dumper,
# installer and power. SAFE=1 builds the restricted variant, which only reads
# state, launches or closes games and manages NeoHEN's own payloads.
ifndef SAFE
COMMON_SRCS += src/api_mem.c src/api_dump.c src/api_saves.c src/api_captures.c src/api_stats.c src/api_trophies.c src/api_games.c src/api_cheats.c src/api_smp.c src/api_store.c src/api_launcher.c src/sqlite_ro.c src/zipstream.c src/discovery.c
FULLDEF     := -DNEOWEB_FULL
else
# the handlers of the routes that are compiled out stay in the sources
FULLDEF     := -Wno-unused-function
endif
HEADERS     := $(wildcard src/*.h)
WEBUI       := webui/index.html
LAUNCHER    := launcher/icon0.png launcher/pic1.png
JXRJS       := jxr/jxr.js

WARN   := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation -Werror
DEFS   := -DLAUNCHER_DIR=\"$(CURDIR)/launcher\" -DWEBUI_PATH=\"$(CURDIR)/$(WEBUI)\" -DJXRJS_PATH=\"$(CURDIR)/$(JXRJS)\" $(FULLDEF)

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

$(ELF): $(COMMON_SRCS) src/plat_ps5.c $(HEADERS) $(WEBUI) $(JXRJS) $(LAUNCHER)
	$(CC) -O2 -g $(WARN) -Isrc $(DEFS) -o $@ $(COMMON_SRCS) src/plat_ps5.c \
	  -lkernel_sys -lSceSystemService -lSceUserService -lSceAppInstUtil

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^
endif

HOST_CC ?= cc

host: $(HOST_BIN)

$(HOST_BIN): $(COMMON_SRCS) src/plat_host.c $(HEADERS) $(WEBUI) $(JXRJS) $(LAUNCHER)
	@mkdir -p build
	$(HOST_CC) -O1 -g $(WARN) -D_GNU_SOURCE -Isrc $(DEFS) -o $@ \
	  $(COMMON_SRCS) src/plat_host.c -lpthread -lm

clean:
	rm -rf $(ELF) build
