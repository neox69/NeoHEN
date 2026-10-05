/* PS5AIO - embedded web interface (usable from a phone as a remote).
 *
 * The page in webui/index.html is linked into the payload with .incbin.
 * A customised copy in /data/ps5aio/webui/ takes precedence, which also
 * allows serving extra static files from that folder.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "api.h"
#include "http.h"
#include "plat.h"

#ifndef WEBUI_PATH
#error "WEBUI_PATH must point to webui/index.html"
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

__asm__(".section .rodata\n"
        ".global ps5aio_webui\n"
        "ps5aio_webui:\n"
        ".incbin \"" WEBUI_PATH "\"\n"
        ".global ps5aio_webui_end\n"
        "ps5aio_webui_end:\n"
        ".byte 0\n"
        ".previous\n");

extern const char ps5aio_webui[];
extern const char ps5aio_webui_end[];

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
    http_send_file(req, local, 0);
    return;
  }
  if(!strcmp(p, "/index.html")) {
    http_send_buf(req, 200, "text/html; charset=utf-8", ps5aio_webui,
                  (size_t)(ps5aio_webui_end - ps5aio_webui));
    return;
  }
  http_send_error(req, 404, "not found: %s", req->path);
}
