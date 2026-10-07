/* NeoWEB - SHA-256.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct sha256 {
  uint32_t h[8];
  uint64_t len;
  uint8_t  buf[64];
  size_t   fill;
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t len);
/* Writes 64 hex digits and a terminator. */
void sha256_final(sha256_t *s, char hex[65]);
