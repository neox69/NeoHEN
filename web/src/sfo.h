/* PS5AIO - param.sfo reader/writer (PS4 titles).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "json.h"

int  sfo_valid(const uint8_t *d, size_t len);
int  sfo_get_str(const uint8_t *d, size_t len, const char *key, char *out, size_t size);
int  sfo_get_int(const uint8_t *d, size_t len, const char *key, uint32_t *out);
/* in-place update; fails if the new value exceeds the entry's max length */
int  sfo_set_str(uint8_t *d, size_t len, const char *key, const char *val);
/* all entries as members of the currently open JSON object */
void sfo_to_json(jw_t *w, const uint8_t *d, size_t len);
