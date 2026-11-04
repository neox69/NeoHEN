/* PS5AIO - tiny threaded HTTP/1.1 server.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "plat.h"

#define IO_CHUNK (1024 * 1024)
#define HTTP_PROBE_SECONDS 20
/* A connection that sends nothing for this long is closed: each one holds a
 * thread, and a browser or a broken client can leave many of them open. */
#define HTTP_IDLE_SECONDS 120
/* Above HTTP_MAX_CONNS (http.h) a connection gets a 503 from the one thread
 * that turns them away; it holds at most this many at a time, each for a
 * second or two, and closes the rest straight away. */
#define HTTP_REJECT_SLOTS 32
#define HTTP_RETRY_AFTER  2
/* the most sendfile() is asked to send in one call */
#define SENDFILE_CHUNK (64 * 1024 * 1024)

/* connections with a thread of their own, open right now */
static int g_conns;

typedef struct conn_arg {
  int            fd;
  http_handler_t handler;
  char           ip[48];
} conn_arg_t;

/* ------------------------------------------------------------ helpers */

const char *
http_status_text(int status) {
  switch(status) {
  case 200: return "OK";
  case 204: return "No Content";
  case 206: return "Partial Content";
  case 304: return "Not Modified";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 409: return "Conflict";
  case 411: return "Length Required";
  case 413: return "Payload Too Large";
  case 416: return "Range Not Satisfiable";
  case 500: return "Internal Server Error";
  case 501: return "Not Implemented";
  case 502: return "Bad Gateway";
  case 503: return "Service Unavailable";
  case 507: return "Insufficient Storage";
  default:  return "Unknown";
  }
}

const char *
http_mime_type(const char *path) {
  const char *ext = strrchr(path, '.');
  if(!ext) {
    return "application/octet-stream";
  }
  ext++;
  if(!strcasecmp(ext, "html") || !strcasecmp(ext, "htm")) return "text/html; charset=utf-8";
  if(!strcasecmp(ext, "js"))   return "application/javascript";
  if(!strcasecmp(ext, "css"))  return "text/css";
  if(!strcasecmp(ext, "json")) return "application/json";
  if(!strcasecmp(ext, "png"))  return "image/png";
  if(!strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg")) return "image/jpeg";
  if(!strcasecmp(ext, "gif"))  return "image/gif";
  if(!strcasecmp(ext, "webp")) return "image/webp";
  if(!strcasecmp(ext, "svg"))  return "image/svg+xml";
  if(!strcasecmp(ext, "dds"))  return "image/vnd-ms.dds";
  if(!strcasecmp(ext, "txt") || !strcasecmp(ext, "log") ||
     !strcasecmp(ext, "ini") || !strcasecmp(ext, "cfg")) return "text/plain; charset=utf-8";
  if(!strcasecmp(ext, "xml"))  return "application/xml";
  if(!strcasecmp(ext, "mp4"))  return "video/mp4";
  if(!strcasecmp(ext, "webm")) return "video/webm";
  if(!strcasecmp(ext, "at9") || !strcasecmp(ext, "wav")) return "audio/wav";
  return "application/octet-stream";
}

static int
hexval(char c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void
url_decode(const char *in, size_t n, char *out, size_t size) {
  size_t o = 0;
  if(!size) {
    return;
  }
  for(size_t i = 0; i < n && o + 1 < size; i++) {
    if(in[i] == '%' && i + 2 < n + 0 && hexval(in[i + 1]) >= 0 &&
       hexval(in[i + 2]) >= 0) {
      out[o++] = (char)(hexval(in[i + 1]) * 16 + hexval(in[i + 2]));
      i += 2;
    } else if(in[i] == '+') {
      out[o++] = ' ';
    } else {
      out[o++] = in[i];
    }
  }
  out[o] = 0;
}

int
http_send_all(int fd, const void *buf, size_t len) {
  const char *p = buf;
  while(len > 0) {
    ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
    if(n < 0) {
      if(errno == EINTR) {
        continue;
      }
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

const char *
http_header(const http_req_t *req, const char *name) {
  for(int i = 0; i < req->nheaders; i++) {
    if(!strcasecmp(req->headers[i].name, name)) {
      return req->headers[i].value;
    }
  }
  return NULL;
}

int
http_query(const http_req_t *req, const char *key, char *out, size_t size) {
  const char *q = req->query;
  size_t klen = strlen(key);

  while(*q) {
    const char *amp = strchr(q, '&');
    size_t seglen = amp ? (size_t)(amp - q) : strlen(q);
    const char *eq = memchr(q, '=', seglen);
    size_t namelen = eq ? (size_t)(eq - q) : seglen;
    char name[128];

    url_decode(q, namelen, name, sizeof(name));
    if(strlen(name) == klen && !memcmp(name, key, klen)) {
      if(eq) {
        url_decode(eq + 1, seglen - namelen - 1, out, size);
      } else if(size) {
        out[0] = 0;
      }
      return 1;
    }
    if(!amp) {
      break;
    }
    q = amp + 1;
  }
  if(size) {
    out[0] = 0;
  }
  return 0;
}

int64_t
http_query_int(const http_req_t *req, const char *key, int64_t def) {
  char buf[64];
  if(!http_query(req, key, buf, sizeof(buf)) || !buf[0]) {
    return def;
  }
  return strtoll(buf, NULL, 0);
}

uint64_t
http_query_u64(const http_req_t *req, const char *key, uint64_t def) {
  char buf[64];
  if(!http_query(req, key, buf, sizeof(buf)) || !buf[0]) {
    return def;
  }
  return strtoull(buf, NULL, 0);
}

/* ------------------------------------------------------------ body */

ssize_t
http_read_body(http_req_t *req, void *buf, size_t size) {
  if(req->body_left <= 0 || size == 0) {
    return 0;
  }
  if((int64_t)size > req->body_left) {
    size = (size_t)req->body_left;
  }

  /* serve buffered bytes first */
  if(req->buf_pos < req->buf_len) {
    size_t avail = req->buf_len - req->buf_pos;
    if(avail > size) {
      avail = size;
    }
    memcpy(buf, req->buf + req->buf_pos, avail);
    req->buf_pos += avail;
    req->body_left -= (int64_t)avail;
    return (ssize_t)avail;
  }

  for(;;) {
    ssize_t n = recv(req->fd, buf, size, 0);
    if(n < 0 && errno == EINTR) {
      continue;
    }
    if(n <= 0) {
      req->keep_alive = 0;
      return -1;
    }
    req->body_left -= n;
    return n;
  }
}

char *
http_read_body_all(http_req_t *req, size_t max, size_t *len_out) {
  size_t len = 0;
  size_t cap;
  char *data;

  if(req->content_length < 0) {
    req->content_length = 0;
  }
  if((size_t)req->content_length > max) {
    return NULL;
  }
  cap = (size_t)req->content_length + 1;
  if(!(data = malloc(cap))) {
    return NULL;
  }
  while(req->body_left > 0) {
    ssize_t n = http_read_body(req, data + len, cap - 1 - len);
    if(n <= 0) {
      free(data);
      return NULL;
    }
    len += (size_t)n;
  }
  data[len] = 0;
  if(len_out) {
    *len_out = len;
  }
  return data;
}

jval_t *
http_read_json(http_req_t *req) {
  size_t len = 0;
  char *body = http_read_body_all(req, 8 * 1024 * 1024, &len);
  jval_t *v;

  if(!body) {
    return NULL;
  }
  if(len == 0) {
    free(body);
    return json_parse("{}", 2);
  }
  v = json_parse(body, len);
  free(body);
  return v;
}

void
http_drain_body(http_req_t *req) {
  char tmp[8192];
  while(req->body_left > 0) {
    if(http_read_body(req, tmp, sizeof(tmp)) <= 0) {
      break;
    }
  }
}

/* ------------------------------------------------------------ responses */

void
http_send_head(http_req_t *req, int status, const char *ctype, int64_t length,
               const char *extra_headers) {
  char head[2048];
  int n;

  /* if the client sent a body we did not consume, we can't keep the
   * connection alive cheaply when it's large */
  if(req->body_left > 1024 * 1024) {
    req->keep_alive = 0;
  }

  /* No Access-Control-* headers: the API only answers pages it served
   * itself (same_origin() in api.c), which need none. */
  if(length < 0 && status != 204 && status != 304) {
    req->keep_alive = 0;            /* the body ends when the connection does */
  }
  n = snprintf(head, sizeof(head),
               "HTTP/1.1 %d %s\r\n"
               "Server: PS5AIO\r\n"
               "Connection: %s\r\n",
               status, http_status_text(status),
               req->keep_alive ? "keep-alive" : "close");
  if(ctype) {
    n += snprintf(head + n, sizeof(head) - (size_t)n, "Content-Type: %s\r\n", ctype);
  }
  if(length >= 0) {
    n += snprintf(head + n, sizeof(head) - (size_t)n,
                  "Content-Length: %" PRId64 "\r\n", length);
  }
  if(extra_headers) {
    n += snprintf(head + n, sizeof(head) - (size_t)n, "%s", extra_headers);
  }
  n += snprintf(head + n, sizeof(head) - (size_t)n, "\r\n");
  if(n > (int)sizeof(head)) {
    n = (int)sizeof(head);
  }
  req->responded = 1;
  if(http_send_all(req->fd, head, (size_t)n)) {
    req->keep_alive = 0;
  }
}

void
http_send_buf(http_req_t *req, int status, const char *ctype, const void *data,
              size_t len) {
  http_send_buf_ex(req, status, ctype, data, len, NULL);
}

void
http_send_buf_ex(http_req_t *req, int status, const char *ctype, const void *data,
                 size_t len, const char *extra_headers) {
  http_send_head(req, status, ctype, (int64_t)len, extra_headers);
  if(len && strcmp(req->method, "HEAD")) {
    if(http_send_all(req->fd, data, len)) {
      req->keep_alive = 0;
    }
  }
}

void
http_send_json(http_req_t *req, int status, jw_t *w) {
  if(w->oom) {
    http_send_error(req, 500, "out of memory");
    return;
  }
  http_send_buf(req, status, "application/json; charset=utf-8",
                w->data ? w->data : "{}", w->data ? w->len : 2);
}

void
http_send_ok(http_req_t *req) {
  static const char ok[] = "{\"ok\":true}";
  http_send_buf(req, 200, "application/json; charset=utf-8", ok, sizeof(ok) - 1);
}

void
http_send_error(http_req_t *req, int status, const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  jw_t w;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  jw_init(&w);
  jw_obj(&w);
  jw_kbool(&w, "ok", 0);
  jw_kstr(&w, "error", msg);
  jw_obj_end(&w);
  http_send_buf(req, status, "application/json; charset=utf-8", w.data, w.len);
  jw_free(&w);
}

/* the next comma separated item of a header, without the spaces around it */
static const char *
next_item(const char *p, const char **item, size_t *len) {
  const char *e;
  while(*p == ' ' || *p == '\t' || *p == ',') {
    p++;
  }
  if(!*p) {
    return NULL;
  }
  e = strchr(p, ',');
  if(!e) {
    e = p + strlen(p);
  }
  *item = p;
  *len = (size_t)(e - p);
  while(*len && (p[*len - 1] == ' ' || p[*len - 1] == '\t')) {
    (*len)--;
  }
  return e;
}

int
http_etag_match(const http_req_t *req, const char *etag) {
  const char *h = http_header(req, "If-None-Match");
  const char *item;
  size_t len, elen = strlen(etag);

  if(!h) {
    return 0;
  }
  while((h = next_item(h, &item, &len))) {
    if(len == 1 && item[0] == '*') {
      return 1;
    }
    if(len > 2 && !strncmp(item, "W/", 2)) {      /* weak comparison */
      item += 2;
      len -= 2;
    }
    if(len == elen && !memcmp(item, etag, len)) {
      return 1;
    }
  }
  return 0;
}

int
http_accepts_gzip(const http_req_t *req) {
  const char *h = http_header(req, "Accept-Encoding");
  const char *item;
  size_t len;
  int star = 0;

  if(!h) {
    return 0;
  }
  while((h = next_item(h, &item, &len))) {
    size_t name = 0;
    int zero = 0;
    while(name < len && item[name] != ';' && item[name] != ' ') {
      name++;
    }
    /* "gzip;q=0" says no; any other weight says yes */
    for(size_t i = name; i + 1 < len; i++) {
      if((item[i] == 'q' || item[i] == 'Q') && item[i + 1] == '=') {
        zero = strtod(item + i + 2, NULL) <= 0;
        break;
      }
    }
    if((name == 4 && !strncasecmp(item, "gzip", 4)) ||
       (name == 6 && !strncasecmp(item, "x-gzip", 6))) {
      return !zero;
    }
    if(name == 1 && item[0] == '*') {
      star = !zero;
    }
  }
  return star;
}

static int
parse_range(const char *hdr, int64_t size, int64_t *start, int64_t *end) {
  /* bytes=a-b | bytes=a- | bytes=-n */
  const char *p;
  char *ep;

  if(!hdr || strncasecmp(hdr, "bytes=", 6)) {
    return 0;
  }
  p = hdr + 6;
  if(*p == '-') {
    int64_t n = strtoll(p + 1, &ep, 10);
    if(n <= 0) {
      return -1;
    }
    *start = n > size ? 0 : size - n;
    *end = size - 1;
    return 1;
  }
  *start = strtoll(p, &ep, 10);
  if(*ep != '-') {
    return -1;
  }
  p = ep + 1;
  if(*p && *p != ',') {
    *end = strtoll(p, &ep, 10);
  } else {
    *end = size - 1;
  }
  if(*end >= size) {
    *end = size - 1;
  }
  if(*start < 0 || *start > *end || *start >= size) {
    return -1;
  }
  return 1;
}

/* Sends len bytes of fd from start: sendfile() where the platform has it,
 * which spares the copy through a buffer, and read()/send() where it does
 * not or the file system will not take it. */
static int
send_range(int sock, int fd, int64_t start, int64_t len) {
  char *buf;

  while(len > 0) {
    size_t want = len > SENDFILE_CHUNK ? SENDFILE_CHUNK : (size_t)len;
    ssize_t n = plat_sendfile(sock, fd, (off_t)start, want);
    if(n > 0) {
      start += n;
      len -= n;
      continue;
    }
    if(n < 0 && errno == EINTR) {
      continue;
    }
    if(n < 0 && (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP ||
                 errno == ENOTSOCK || errno == ENOTSUP)) {
      break;                        /* the copy below does it */
    }
    return -1;                      /* the peer went away, or the file shrank */
  }
  if(len <= 0) {
    return 0;
  }

  if(!(buf = malloc(IO_CHUNK))) {
    return -1;
  }
  if(lseek(fd, (off_t)start, SEEK_SET) < 0) {
    free(buf);
    return -1;
  }
  while(len > 0) {
    size_t want = len > IO_CHUNK ? IO_CHUNK : (size_t)len;
    ssize_t n = read(fd, buf, want);
    if(n <= 0 || http_send_all(sock, buf, (size_t)n)) {
      free(buf);
      return -1;
    }
    len -= n;
  }
  free(buf);
  return 0;
}

void
http_send_file(http_req_t *req, const char *path, int as_attachment) {
  http_send_file_ex(req, path, as_attachment, NULL);
}

void
http_send_file_ex(http_req_t *req, const char *path, int as_attachment,
                  const char *cache_control) {
  struct stat st;
  int64_t start = 0, end;
  int partial = 0;
  char extra[1024], etag[64], cache[160];
  int fd;

  if((fd = open(path, O_RDONLY)) < 0) {
    http_send_error(req, errno == ENOENT ? 404 : 403, "%s: %s", path, strerror(errno));
    return;
  }
  if(fstat(fd, &st) || S_ISDIR(st.st_mode)) {
    close(fd);
    http_send_error(req, 400, "%s: not a regular file", path);
    return;
  }

  /* size and time of the last change: enough to tell one version from the
   * next, without reading the file */
  snprintf(etag, sizeof(etag), "\"%" PRIx64 "-%" PRIx64 "\"",
           (uint64_t)st.st_size, (uint64_t)st.st_mtime);
  snprintf(cache, sizeof(cache), "ETag: %s\r\n%s%s%s", etag,
           cache_control ? "Cache-Control: " : "", cache_control ? cache_control : "",
           cache_control ? "\r\n" : "");
  if(http_etag_match(req, etag)) {
    close(fd);
    http_send_head(req, 304, NULL, -1, cache);
    return;
  }

  end = (int64_t)st.st_size - 1;
  if(st.st_size > 0) {
    int r = parse_range(http_header(req, "Range"), (int64_t)st.st_size, &start, &end);
    if(r < 0) {
      close(fd);
      snprintf(extra, sizeof(extra), "Content-Range: bytes */%" PRId64 "\r\n",
               (int64_t)st.st_size);
      http_send_head(req, 416, NULL, 0, extra);
      return;
    }
    partial = r > 0;
  }

  {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    int n = snprintf(extra, sizeof(extra), "Accept-Ranges: bytes\r\n%s", cache);
    if(partial) {
      n += snprintf(extra + n, sizeof(extra) - (size_t)n,
                    "Content-Range: bytes %" PRId64 "-%" PRId64 "/%" PRId64 "\r\n",
                    start, end, (int64_t)st.st_size);
    }
    if(as_attachment) {
      /* a file name may hold anything but '/': a quote or a line break
       * must not end the header */
      char name[256];
      size_t i;
      for(i = 0; base[i] && i + 1 < sizeof(name); i++) {
        name[i] = base[i] == '"' || base[i] == '\\' || (unsigned char)base[i] < 0x20 ? '_' : base[i];
      }
      name[i] = 0;
      snprintf(extra + n, sizeof(extra) - (size_t)n,
               "Content-Disposition: attachment; filename=\"%s\"\r\n", name);
    }
  }

  int64_t len = st.st_size > 0 ? end - start + 1 : 0;
  http_send_head(req, partial ? 206 : 200, http_mime_type(path), len, extra);
  if(!strcmp(req->method, "HEAD") || len == 0) {
    close(fd);
    return;
  }

  if(send_range(req->fd, fd, start, len)) {
    req->keep_alive = 0;
  }
  close(fd);
}

/* ------------------------------------------------------------ request parsing */

/* find "\r\n" (the PS5 libc has no memmem) */
static char *
find_crlf(char *p, size_t n) {
  for(size_t i = 0; i + 1 < n; i++) {
    if(p[i] == '\r' && p[i + 1] == '\n') {
      return p + i;
    }
  }
  return NULL;
}

static void
free_headers(http_req_t *req) {
  for(int i = 0; i < req->nheaders; i++) {
    free(req->headers[i].name);
    free(req->headers[i].value);
  }
  req->nheaders = 0;
}

/* read until "\r\n\r\n"; leftover bytes stay in buffer. returns header length */
static ssize_t
read_head(http_req_t *req) {
  /* shift any pipelined leftover to the front */
  if(req->buf_pos > 0) {
    memmove(req->buf, req->buf + req->buf_pos, req->buf_len - req->buf_pos);
    req->buf_len -= req->buf_pos;
    req->buf_pos = 0;
  }

  for(;;) {
    if(req->buf_len >= 4) {
      for(size_t i = 0; i + 3 < req->buf_len; i++) {
        if(!memcmp(req->buf + i, "\r\n\r\n", 4)) {
          return (ssize_t)(i + 4);
        }
      }
    }
    if(req->buf_len >= sizeof(req->buf) - 1) {
      return -2;
    }
    ssize_t n = recv(req->fd, req->buf + req->buf_len,
                     sizeof(req->buf) - 1 - req->buf_len, 0);
    if(n < 0 && errno == EINTR) {
      continue;
    }
    if(n <= 0) {
      return -1;
    }
    req->buf_len += (size_t)n;
  }
}

static int
parse_head(http_req_t *req, size_t head_len) {
  char *line = req->buf;
  char *end = req->buf + head_len;
  char *eol;
  char target[sizeof(req->path) + sizeof(req->query)];

  if(!(eol = find_crlf(line, (size_t)(end - line)))) {
    return -1;
  }
  *eol = 0;
  if(sscanf(line, "%15s %10239s", req->method, target) != 2) {
    return -1;
  }

  char *q = strchr(target, '?');
  if(q) {
    *q = 0;
    snprintf(req->query, sizeof(req->query), "%s", q + 1);
  } else {
    req->query[0] = 0;
  }
  url_decode(target, strlen(target), req->path, sizeof(req->path));

  req->keep_alive = strstr(line, "HTTP/1.1") != NULL;
  req->content_length = -1;

  line = eol + 2;
  while(line < end - 2) {
    if(!(eol = find_crlf(line, (size_t)(end - line)))) {
      break;
    }
    *eol = 0;
    char *colon = strchr(line, ':');
    if(colon) {
      /* too many headers is an error, not a cut: the ones dropped could be
       * Origin or Host, which decide whether the request is let in */
      if(req->nheaders >= HTTP_MAX_HEADERS) {
        return -1;
      }
      *colon = 0;
      char *val = colon + 1;
      while(*val == ' ' || *val == '\t') {
        val++;
      }
      char *n = strdup(line), *v = strdup(val);
      if(!n || !v) {
        free(n);
        free(v);
        return -1;
      }
      req->headers[req->nheaders].name = n;
      req->headers[req->nheaders].value = v;
      req->nheaders++;
    }
    line = eol + 2;
  }

  const char *cl = http_header(req, "Content-Length");
  if(cl) {
    req->content_length = strtoll(cl, NULL, 10);
  }
  req->body_left = req->content_length > 0 ? req->content_length : 0;

  const char *conn = http_header(req, "Connection");
  if(conn) {
    if(!strcasecmp(conn, "close")) {
      req->keep_alive = 0;
    } else if(!strcasecmp(conn, "keep-alive")) {
      req->keep_alive = 1;
    }
  }
  if(http_header(req, "Transfer-Encoding")) {
    /* chunked request bodies are not supported */
    return -2;
  }

  req->buf_pos = head_len;
  return 0;
}

static void *
conn_thread(void *arg) {
  conn_arg_t *ca = arg;
  http_req_t *req = calloc(1, sizeof(http_req_t));

  if(!req) {
    close(ca->fd);
    free(ca);
    __atomic_sub_fetch(&g_conns, 1, __ATOMIC_SEQ_CST);
    return NULL;
  }
  req->fd = ca->fd;
  snprintf(req->client_ip, sizeof(req->client_ip), "%s", ca->ip);

  for(;;) {
    ssize_t hl = read_head(req);
    if(hl < 0) {
      if(hl == -2) {
        req->keep_alive = 0;
        http_send_error(req, 413, "request header too large");
      }
      break;
    }

    int pr = parse_head(req, (size_t)hl);
    if(pr) {
      req->keep_alive = 0;
      http_send_error(req, pr == -2 ? 501 : 400,
                      pr == -2 ? "chunked bodies not supported" : "bad request");
      break;
    }

    req->responded = 0;
    if(!strcmp(req->method, "OPTIONS")) {
      /* what the server takes; a cross-origin preflight finds no
       * Access-Control-Allow-* here and the browser stops there */
      http_send_head(req, 204, NULL, -1, "Allow: GET, HEAD, POST, PUT, OPTIONS\r\n");
    } else {
      ca->handler(req);
      if(!req->responded) {
        http_send_error(req, 500, "no response");
      }
    }

    if(req->body_left > 0) {
      if(req->body_left > 4 * 1024 * 1024) {
        req->keep_alive = 0;
      } else {
        http_drain_body(req);
      }
    }
    free_headers(req);
    if(!req->keep_alive) {
      break;
    }
    /* keep leftover pipelined bytes */
    memset(req->method, 0, sizeof(req->method));
  }

  free_headers(req);
  shutdown(req->fd, SHUT_RDWR);
  close(req->fd);
  free(req);
  free(ca);
  __atomic_sub_fetch(&g_conns, 1, __ATOMIC_SEQ_CST);
  return NULL;
}

/* Rest Mode kills the listening socket: after the console wakes up accept()
 * either fails for good or blocks on a socket nobody can reach. Both ends in
 * the same place, a new socket. */
static volatile int g_relisten;

static int
open_listener(uint16_t port) {
  struct sockaddr_in addr;
  int one = 1;
  int srv;

  if((srv = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    LOG_ERR("socket: %s", strerror(errno));
    return -1;
  }
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if(bind(srv, (struct sockaddr *)&addr, sizeof(addr))) {
    LOG_ERR("bind(%d): %s", port, strerror(errno));
    close(srv);
    return -1;
  }
  if(listen(srv, 32)) {
    LOG_ERR("listen: %s", strerror(errno));
    close(srv);
    return -1;
  }
  return srv;
}

/* Connects to our own port now and then; two misses in a row mean the
 * listener is dead even though accept() never said so. */
static void *
probe_thread(void *arg) {
  uint16_t port = (uint16_t)(uintptr_t)arg;
  int misses = 0;

  for(;;) {
    struct sockaddr_in addr;
    struct timeval tv = {3, 0};
    int fd;

    sleep(HTTP_PROBE_SECONDS);
    if(g_relisten) {
      continue;
    }
    if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
      continue;
    }
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if(connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
      if(++misses >= 2) {
        LOG_WARN("port %d does not answer (%s); listening again", port,
                 strerror(errno));
        g_relisten = 1;
        misses = 0;
      }
    } else {
      misses = 0;
    }
    close(fd);
  }
  return NULL;
}

/* ------------------------------------------------------------ busy */

/* Connections over the cap are passed here through a pipe, so accept() goes
 * on at once. This thread waits a moment for the request, answers 503 and
 * reads what is left until the client hangs up: closing a socket with unread
 * data resets it, and the client could lose the answer. */
static int g_reject_pipe[2] = {-1, -1};

static int64_t
now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
drain(int fd, int *eof) {
  char tmp[2048];
  for(;;) {
    ssize_t n = recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
    if(n > 0) {
      continue;
    }
    if(n < 0 && errno == EINTR) {
      continue;
    }
    *eof = n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK);
    return;
  }
}

static void *
reject_thread(void *arg) {
  static const char body[] = "{\"ok\":false,\"error\":\"too many connections\"}";
  char busy[320];
  int blen;
  struct {
    int     fd;
    int     answered;
    int64_t deadline;
  } slot[HTTP_REJECT_SLOTS];
  struct pollfd pfd[HTTP_REJECT_SLOTS + 1];
  int n = 0;
  (void)arg;

  blen = snprintf(busy, sizeof(busy),
                  "HTTP/1.1 503 Service Unavailable\r\n"
                  "Server: PS5AIO\r\n"
                  "Retry-After: %d\r\n"
                  "Content-Type: application/json; charset=utf-8\r\n"
                  "Content-Length: %u\r\n"
                  "Connection: close\r\n"
                  "\r\n%s",
                  HTTP_RETRY_AFTER, (unsigned)(sizeof(body) - 1), body);
  for(;;) {
    int64_t now = now_ms(), wait = 1000;
    for(int i = 0; i < n; i++) {
      int64_t left = slot[i].deadline - now;
      if(left < wait) {
        wait = left < 0 ? 0 : left;
      }
      pfd[i + 1].fd = slot[i].fd;
      pfd[i + 1].events = POLLIN;
      pfd[i + 1].revents = 0;
    }
    pfd[0].fd = g_reject_pipe[0];
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;
    if(poll(pfd, (nfds_t)n + 1, (int)wait) < 0 && errno != EINTR) {
      sleep(1);
      continue;
    }

    now = now_ms();
    for(int i = 0; i < n; i++) {
      int eof = 0;
      if(!(pfd[i + 1].revents & (POLLIN | POLLHUP | POLLERR)) && now < slot[i].deadline) {
        continue;
      }
      drain(slot[i].fd, &eof);
      if(!slot[i].answered && !eof) {
        /* the request is in (or it is late): answer, then read until the
         * client closes, for at most two seconds */
        if(send(slot[i].fd, busy, (size_t)blen, MSG_DONTWAIT | MSG_NOSIGNAL) < 0) {
          eof = 1;
        } else {
          shutdown(slot[i].fd, SHUT_WR);
          slot[i].answered = 1;
          slot[i].deadline = now + 2000;
        }
      } else if(now >= slot[i].deadline) {
        eof = 1;
      }
      if(eof) {
        close(slot[i].fd);
        slot[i] = slot[n - 1];
        pfd[i + 1] = pfd[n];
        n--;
        i--;
      }
    }

    if(pfd[0].revents & POLLIN) {
      int fds[16];
      ssize_t got = read(g_reject_pipe[0], fds, sizeof(fds));
      for(ssize_t k = 0; k < got / (ssize_t)sizeof(int); k++) {
        if(n >= HTTP_REJECT_SLOTS) {
          close(fds[k]);
          continue;
        }
        slot[n].fd = fds[k];
        slot[n].answered = 0;
        slot[n].deadline = now + 1000;
        n++;
      }
    }
  }
  return NULL;
}

static void
reject_start(void) {
  pthread_t th;
  pthread_attr_t attr;

  if(pipe(g_reject_pipe)) {
    g_reject_pipe[0] = g_reject_pipe[1] = -1;
    return;
  }
  fcntl(g_reject_pipe[1], F_SETFL, fcntl(g_reject_pipe[1], F_GETFL) | O_NONBLOCK);
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, 64 * 1024);
  if(pthread_create(&th, &attr, reject_thread, NULL)) {
    close(g_reject_pipe[0]);
    close(g_reject_pipe[1]);
    g_reject_pipe[0] = g_reject_pipe[1] = -1;
  }
  pthread_attr_destroy(&attr);
}

/* hands a connection over the cap to reject_thread(), or closes it when
 * that one is full too */
static void
reject(int fd) {
  if(g_reject_pipe[1] < 0 || write(g_reject_pipe[1], &fd, sizeof(fd)) != sizeof(fd)) {
    close(fd);
  }
}

int
http_serve(uint16_t port, http_handler_t handler) {
  int one = 1;
  int srv;
  pthread_t probe;

  signal(SIGPIPE, SIG_IGN);

  if((srv = open_listener(port)) < 0) {
    return -1;
  }
  LOG_INFO("HTTP server listening on port %d", port);

  if(!pthread_create(&probe, NULL, probe_thread, (void *)(uintptr_t)port)) {
    pthread_detach(probe);
  }
  reject_start();

  for(;;) {
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    struct pollfd pfd = {.fd = srv, .events = POLLIN};
    int fd = -1;
    int dead = g_relisten;

    if(!dead) {
      int pr = poll(&pfd, 1, 1000);
      if(pr == 0 || (pr < 0 && errno == EINTR)) {
        continue;
      }
      if(pr < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        LOG_WARN("listener lost (%s); listening again",
                 pr < 0 ? strerror(errno) : "poll");
        dead = 1;
      }
    }
    if(!dead) {
      fd = accept(srv, (struct sockaddr *)&caddr, &clen);
      if(fd < 0) {
        if(errno == EINTR || errno == ECONNABORTED || errno == EAGAIN) {
          continue;
        }
        LOG_WARN("accept: %s; listening again", strerror(errno));
        dead = 1;
      }
    }
    if(dead) {
      close(srv);
      while((srv = open_listener(port)) < 0) {
        sleep(2);
      }
      g_relisten = 0;
      LOG_INFO("HTTP server listening on port %d again", port);
      continue;
    }

    /* each connection is a thread with a 512 KB stack: past the cap the
     * client is told to come back in a moment */
    if(__atomic_load_n(&g_conns, __ATOMIC_SEQ_CST) >= HTTP_MAX_CONNS) {
      static int64_t last_warn;
      if(now_ms() - last_warn > 10000) {
        last_warn = now_ms();
        LOG_WARN("%d connections open; turning new ones away", HTTP_MAX_CONNS);
      }
      reject(fd);
      continue;
    }

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int bufsz = 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
    struct timeval idle = {HTTP_IDLE_SECONDS, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof(idle));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &idle, sizeof(idle));

    conn_arg_t *ca = calloc(1, sizeof(conn_arg_t));
    if(!ca) {
      close(fd);
      continue;
    }
    ca->fd = fd;
    ca->handler = handler;
    inet_ntop(AF_INET, &caddr.sin_addr, ca->ip, sizeof(ca->ip));

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    __atomic_add_fetch(&g_conns, 1, __ATOMIC_SEQ_CST);
    if(pthread_create(&th, &attr, conn_thread, ca)) {
      __atomic_sub_fetch(&g_conns, 1, __ATOMIC_SEQ_CST);
      close(fd);
      free(ca);
    }
    pthread_attr_destroy(&attr);
  }
}
