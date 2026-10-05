/* PS5AIO - param.sfo reader/writer (PS4 titles).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "sfo.h"

#include <string.h>

#define SFO_MAGIC 0x46535000u /* "\0PSF" little endian */
#define SFO_FMT_UTF8S 0x0004
#define SFO_FMT_UTF8  0x0204
#define SFO_FMT_INT32 0x0404

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct sfo_entry {
  const char *key;
  uint16_t    fmt;
  uint32_t    len;
  uint32_t    max_len;
  uint32_t    data_off; /* absolute offset */
} sfo_entry_t;

static int
sfo_entry(const uint8_t *d, size_t len, uint32_t i, sfo_entry_t *e) {
  uint32_t key_tab = rd32(d + 8);
  uint32_t data_tab = rd32(d + 12);
  size_t off = 20 + (size_t)i * 16;

  if(off + 16 > len) {
    return -1;
  }
  uint32_t key_off = key_tab + rd16(d + off);
  if(key_off >= len) {
    return -1;
  }
  e->key = (const char *)d + key_off;
  if(!memchr(e->key, 0, len - key_off)) {
    return -1;
  }
  e->fmt = rd16(d + off + 2);
  e->len = rd32(d + off + 4);
  e->max_len = rd32(d + off + 8);
  e->data_off = data_tab + rd32(d + off + 12);
  if((size_t)e->data_off + e->max_len > len || e->len > e->max_len) {
    return -1;
  }
  return 0;
}

int
sfo_valid(const uint8_t *d, size_t len) {
  return d && len >= 20 && rd32(d) == SFO_MAGIC && rd32(d + 16) < 4096;
}

static int
sfo_find(const uint8_t *d, size_t len, const char *key, sfo_entry_t *out) {
  if(!sfo_valid(d, len)) {
    return -1;
  }
  uint32_t n = rd32(d + 16);
  for(uint32_t i = 0; i < n; i++) {
    if(sfo_entry(d, len, i, out)) {
      return -1;
    }
    if(!strcmp(out->key, key)) {
      return 0;
    }
  }
  return -1;
}

int
sfo_get_str(const uint8_t *d, size_t len, const char *key, char *out, size_t size) {
  sfo_entry_t e;
  if(sfo_find(d, len, key, &e) || e.fmt == SFO_FMT_INT32 || !size) {
    return -1;
  }
  size_t n = e.len;
  if(n && d[e.data_off + n - 1] == 0) {
    n--;
  }
  if(n >= size) {
    n = size - 1;
  }
  memcpy(out, d + e.data_off, n);
  out[n] = 0;
  return 0;
}

int
sfo_get_int(const uint8_t *d, size_t len, const char *key, uint32_t *out) {
  sfo_entry_t e;
  if(sfo_find(d, len, key, &e) || e.fmt != SFO_FMT_INT32 || e.len < 4) {
    return -1;
  }
  *out = rd32(d + e.data_off);
  return 0;
}

int
sfo_set_str(uint8_t *d, size_t len, const char *key, const char *val) {
  sfo_entry_t e;
  size_t n = strlen(val) + 1;

  if(sfo_find(d, len, key, &e) || e.fmt == SFO_FMT_INT32 || n > e.max_len) {
    return -1;
  }
  memset(d + e.data_off, 0, e.max_len);
  memcpy(d + e.data_off, val, n);

  /* update the len field of the matching index entry */
  uint32_t cnt = rd32(d + 16);
  for(uint32_t i = 0; i < cnt; i++) {
    sfo_entry_t t;
    if(!sfo_entry(d, len, i, &t) && t.data_off == e.data_off) {
      size_t off = 20 + (size_t)i * 16 + 4;
      d[off] = (uint8_t)n;
      d[off + 1] = (uint8_t)(n >> 8);
      d[off + 2] = (uint8_t)(n >> 16);
      d[off + 3] = (uint8_t)(n >> 24);
      break;
    }
  }
  return 0;
}

void
sfo_to_json(jw_t *w, const uint8_t *d, size_t len) {
  if(!sfo_valid(d, len)) {
    return;
  }
  uint32_t n = rd32(d + 16);
  for(uint32_t i = 0; i < n; i++) {
    sfo_entry_t e;
    if(sfo_entry(d, len, i, &e)) {
      break;
    }
    if(e.fmt == SFO_FMT_INT32 && e.len >= 4) {
      jw_kuint(w, e.key, rd32(d + e.data_off));
    } else if(e.fmt == SFO_FMT_UTF8 || e.fmt == SFO_FMT_UTF8S) {
      size_t sl = strnlen((const char *)d + e.data_off, e.len);
      jw_key(w, e.key);
      jw_strn(w, (const char *)d + e.data_off, sl);
    }
  }
}
