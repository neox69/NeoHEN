/* NeoWEB - a ZIP archive written straight into a reply.
 *
 * Not compressed (what goes in is encrypted or already packed), with ZIP64
 * fields where a size or an offset needs them. Every size is known before
 * the first byte goes out, so the browser gets a Content-Length; each file
 * is read twice, once for its checksum.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "http.h"

typedef struct zentry {
  char    *name;  /* inside the archive */
  char    *local; /* on disk; NULL for text held in `text` */
  char    *text;
  uint64_t size;
  uint64_t offset;
  uint32_t crc;
  uint16_t dos_time, dos_date;
} zentry_t;

typedef struct zlist {
  zentry_t *v;
  size_t n, cap;
} zlist_t;

uint32_t zip_crc32(uint32_t crc, const unsigned char *p, size_t n);

/* One file: from disk (`local`) or the text given; `name` is its path inside. */
int  zlist_add(zlist_t *z, const char *name, const char *local, const char *text, uint64_t size,
               time_t mtime);
/* Every file below a folder, as <prefix>/<path below it>. */
int  zlist_tree(zlist_t *z, const char *local, const char *prefix, int depth);
void zlist_free(zlist_t *z);
void zip_send(http_req_t *req, zlist_t *z, const char *filename);
