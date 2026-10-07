# NeoHEN themes - proof of concept payload.
#
#   make        build neotheme.elf for the PS5 (needs PS5_PAYLOAD_SDK)
#
# SPDX-License-Identifier: GPL-3.0-or-later

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is undefined (export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk))
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

neotheme.elf: src/neotheme.c
	$(CC) -O2 -g -Wall -Wextra -Werror -o $@ $< -lkernel_sys

clean:
	rm -f neotheme.elf
