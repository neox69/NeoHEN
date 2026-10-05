/* PS5AIO - tiny threaded HTTP/1.1 server.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "json.h"

#define HTTP_MAX_HEADERS 32

typedef struct http_header {
  char *name;
  char *value;
} http_header_t;

typedef struct http_req {
  int           fd;
  char          client_ip[48];
  char          method[16];
  char          path[2048];   /* decoded path without query */
  char          query[8192];  /* raw query string */
  http_header_t headers[HTTP_MAX_HEADERS];
  int           nheaders;
  int64_t       content_length; /* -1 when absent */
  int64_t       body_left;
  int           keep_alive;
  int           responded;

  /* receive buffer (headers + possibly start of body) */
  char          buf[16384];
  size_t        buf_len;
  size_t        buf_pos;
} http_req_t;

typedef void (*http_handler_t)(http_req_t *req);

/* serve forever on the given port; returns only on fatal error */
int http_serve(uint16_t port, http_handler_t handler);

const char *http_header(const http_req_t *req, const char *name);

/* query-string accessors (values are url-decoded) */
int         http_query(const http_req_t *req, const char *key, char *out, size_t size);
int64_t     http_query_int(const http_req_t *req, const char *key, int64_t def);
uint64_t    http_query_u64(const http_req_t *req, const char *key, uint64_t def);

/* body accessors */
ssize_t     http_read_body(http_req_t *req, void *buf, size_t size);
char       *http_read_body_all(http_req_t *req, size_t max, size_t *len_out);
jval_t     *http_read_json(http_req_t *req); /* NULL body => empty object */
void        http_drain_body(http_req_t *req);

/* responses */
int  http_send_all(int fd, const void *buf, size_t len);
void http_send_head(http_req_t *req, int status, const char *ctype,
                    int64_t length, const char *extra_headers);
void http_send_buf(http_req_t *req, int status, const char *ctype,
                   const void *data, size_t len);
void http_send_json(http_req_t *req, int status, jw_t *w);
void http_send_ok(http_req_t *req);
void http_send_error(http_req_t *req, int status, const char *fmt, ...)
  __attribute__((format(printf, 3, 4)));
void http_send_file(http_req_t *req, const char *path, int as_attachment);

const char *http_status_text(int status);
const char *http_mime_type(const char *path);
void        url_decode(const char *in, size_t n, char *out, size_t size);
