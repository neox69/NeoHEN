# NeoHEN themes - proof of concept payload.
#
#   make        build neotheme.elf for the PS5 (needs PS5_PAYLOAD_SDK)
#
# SPDX-License-Identifier: GPL-3.0-or-later

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is undefined (export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk))
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

all: neotheme.elf

# the themes in bundled/ go inside the program (see tools/pack_themes.py)
BUNDLED := $(shell find bundled -type f 2>/dev/null | sed 's/ /\\ /g')

build/themes.pack: tools/pack_themes.py $(BUNDLED)
	python3 tools/pack_themes.py bundled $@

neotheme.elf: src/neotheme.c src/pack.S build/themes.pack
	$(CC) -O2 -g -Wall -Wextra -Werror -o $@ src/neotheme.c src/pack.S -lkernel_sys

.PHONY: all clean
clean:
	rm -rf neotheme.elf build
