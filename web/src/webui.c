/* NeoWEB - embedded web interface (usable from a phone as a remote).
 *
 * The page in webui/index.html is linked into the payload with .incbin.
 * A customised copy in /data/neohen/web/webui/ takes precedence, which also
 * allows serving extra static files from that folder.
 *
 * The full build carries a second file, /jxr.js: the JPEG XR decoder the
 * page asks for when there are HDR screenshots to show (see web/jxr).
 *
 * Both are linked gzipped (the Makefile compresses them: the 424 KB page is
 * 135 KB on the wire, the 214 KB of jxr.js are 71 KB), and that goes to every
 * client that takes gzip, which is every browser. The rare one that does not
 * gets a copy
 * inflated for it with miniz: keeping a plain copy as well would cost the
 * payload half a megabyte of memory for curl. The ETag is the hash of the
 * file, so the browser asks again each time (no-cache) and gets a 304.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "api.h"
#include "http.h"
#include "plat.h"
#include "webui_etag.h"                 /* made by the Makefile */
#include "../../third_party/cheat_support/miniz.h"

#ifndef WEBUI_GZ_PATH
#error "WEBUI_GZ_PATH must point to the gzipped webui/index.html"
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

__asm__(".section .rodata\n"
        ".global ps5aio_webui\n"
        "ps5aio_webui:\n"
        ".incbin \"" WEBUI_GZ_PATH "\"\n"
        ".global ps5aio_webui_end\n"
        "ps5aio_webui_end:\n"
        ".byte 0\n"
        ".previous\n");

extern const char ps5aio_webui[];
extern const char ps5aio_webui_end[];

#ifdef NEOWEB_FULL
__asm__(".section .rodata\n"
        ".global ps5aio_jxrjs\n"
        "ps5aio_jxrjs:\n"
        ".incbin \"" JXRJS_GZ_PATH "\"\n"
        ".global ps5aio_jxrjs_end\n"
        "ps5aio_jxrjs_end:\n"
        ".byte 0\n"
        ".previous\n");

extern const char ps5aio_jxrjs[];
extern const char ps5aio_jxrjs_end[];
#endif

/* where the deflate stream of a gzip file starts, 0 when it is not one */
static size_t
gzip_start(const unsigned char *gz, size_t len) {
  size_t p = 10;
  unsigned flags;

  if(len < 18 || gz[0] != 0x1f || gz[1] != 0x8b || gz[2] != 8) {
    return 0;
  }
  flags = gz[3];
  if(flags & 4) {                           /* FEXTRA */
    if(p + 2 > len) {
      return 0;
    }
    p += 2 + (size_t)(gz[p] | gz[p + 1] << 8);
  }
  for(unsigned f = 8; f <= 16; f <<= 1) {   /* FNAME, FCOMMENT */
    if(flags & f) {
      while(p < len && gz[p]) {
        p++;
      }
      p++;
    }
  }
  if(flags & 2) {                           /* FHCRC */
    p += 2;
  }
  return p + 8 <= len ? p : 0;
}

static void
send_embedded(http_req_t *req, const char *ctype, const char *gz, const char *gz_end,
              const char *hash) {
  size_t len = (size_t)(gz_end - gz);
  int gzip = http_accepts_gzip(req);
  char etag[64], extra[256];

  /* one ETag for each encoding: they are different bytes */
  snprintf(etag, sizeof(etag), "\"%s%s\"", hash, gzip ? "-gz" : "");
  snprintf(extra, sizeof(extra), "ETag: %s\r\nCache-Control: no-cache\r\n"
           "Vary: Accept-Encoding\r\n", etag);
  if(http_etag_match(req, etag)) {
    http_send_head(req, 304, NULL, -1, extra);
    return;
  }
  if(gzip) {
    snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
             "Content-Encoding: gzip\r\n");
    http_send_buf_ex(req, 200, ctype, gz, len, extra);
    return;
  }

  const unsigned char *u = (const unsigned char *)gz;
  size_t start = gzip_start(u, len);
  size_t size = start ? (size_t)(u[len - 4] | u[len - 3] << 8 | u[len - 2] << 16 |
                                 (uint32_t)u[len - 1] << 24) : 0;
  char *plain = size ? malloc(size) : NULL;
  if(!plain) {
    http_send_error(req, 500, "the page could not be unpacked");
    return;
  }
  if(tinfl_decompress_mem_to_mem(plain, size, u + start, len - start - 8, 0) != size) {
    free(plain);
    http_send_error(req, 500, "the page could not be unpacked");
    return;
  }
  http_send_buf_ex(req, 200, ctype, plain, size, extra);
  free(plain);
}

void
webui_serve(http_req_t *req) {
  char console[PATH_MAX], local[PATH_MAX];
  struct stat st;
  const char *p = req->path;

  if(strcmp(req->method, "GET") && strcmp(req->method, "HEAD")) {
    http_send_error(req, 405, "method not allowed");
    return;
  }
  if(!strcmp(p, "/") || !strcmp(p, "/index.html")) {
    p = "/index.html";
  }
  if(strstr(p, "..")) {
    http_send_error(req, 400, "bad path");
    return;
  }

  snprintf(console, sizeof(console), PS5AIO_DATA_DIR "/webui%s", p);
  plat_path(console, local, sizeof(local));
  if(!stat(local, &st) && S_ISREG(st.st_mode)) {
    http_send_file_ex(req, local, 0, "no-cache");
    return;
  }
  if(!strcmp(p, "/index.html")) {
    send_embedded(req, "text/html; charset=utf-8", ps5aio_webui, ps5aio_webui_end,
                  WEBUI_ETAG);
    return;
  }
#ifdef NEOWEB_FULL
  if(!strcmp(p, "/jxr.js")) {
    send_embedded(req, "application/javascript; charset=utf-8", ps5aio_jxrjs,
                  ps5aio_jxrjs_end, JXRJS_ETAG);
    return;
  }
#endif
  http_send_error(req, 404, "not found: %s", req->path);
}
