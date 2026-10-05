/* PS5AIO - tiny threaded HTTP/1.1 server.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"

#define IO_CHUNK (1024 * 1024)

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
  case 503: return "Service Unavailable";
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

  n = snprintf(head, sizeof(head),
               "HTTP/1.1 %d %s\r\n"
               "Server: PS5AIO\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "Access-Control-Allow-Headers: *\r\n"
               "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
               "Connection: %s\r\n",
               status, http_status_text(status),
               req->keep_alive ? "keep-alive" : "close");
  if(ctype) {
    n += snprintf(head + n, sizeof(head) - (size_t)n, "Content-Type: %s\r\n", ctype);
  }
  if(length >= 0) {
    n += snprintf(head + n, sizeof(head) - (size_t)n,
                  "Content-Length: %" PRId64 "\r\n", length);
  } else {
    req->keep_alive = 0;
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
  http_send_head(req, status, ctype, (int64_t)len, NULL);
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

void
http_send_file(http_req_t *req, const char *path, int as_attachment) {
  struct stat st;
  int64_t start = 0, end;
  int partial = 0;
  char extra[1024];
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
    int n = snprintf(extra, sizeof(extra), "Accept-Ranges: bytes\r\n");
    if(partial) {
      n += snprintf(extra + n, sizeof(extra) - (size_t)n,
                    "Content-Range: bytes %" PRId64 "-%" PRId64 "/%" PRId64 "\r\n",
                    start, end, (int64_t)st.st_size);
    }
    if(as_attachment) {
      snprintf(extra + n, sizeof(extra) - (size_t)n,
               "Content-Disposition: attachment; filename=\"%s\"\r\n", base);
    }
  }

  int64_t len = st.st_size > 0 ? end - start + 1 : 0;
  http_send_head(req, partial ? 206 : 200, http_mime_type(path), len, extra);
  if(!strcmp(req->method, "HEAD") || len == 0) {
    close(fd);
    return;
  }

  char *buf = malloc(IO_CHUNK);
  if(!buf) {
    close(fd);
    req->keep_alive = 0;
    return;
  }
  if(lseek(fd, start, SEEK_SET) < 0) {
    req->keep_alive = 0;
  } else {
    while(len > 0) {
      size_t want = len > IO_CHUNK ? IO_CHUNK : (size_t)len;
      ssize_t n = read(fd, buf, want);
      if(n <= 0 || http_send_all(req->fd, buf, (size_t)n)) {
        req->keep_alive = 0;
        break;
      }
      len -= n;
    }
  }
  free(buf);
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
  while(line < end - 2 && req->nheaders < HTTP_MAX_HEADERS) {
    if(!(eol = find_crlf(line, (size_t)(end - line)))) {
      break;
    }
    *eol = 0;
    char *colon = strchr(line, ':');
    if(colon) {
      *colon = 0;
      char *val = colon + 1;
      while(*val == ' ' || *val == '\t') {
        val++;
      }
      req->headers[req->nheaders].name = strdup(line);
      req->headers[req->nheaders].value = strdup(val);
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
      http_send_head(req, 204, NULL, 0, NULL);
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
  return NULL;
}

int
http_serve(uint16_t port, http_handler_t handler) {
  struct sockaddr_in addr;
  int one = 1;
  int srv;

  signal(SIGPIPE, SIG_IGN);

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

  LOG_INFO("HTTP server listening on port %d", port);

  for(;;) {
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    int fd = accept(srv, (struct sockaddr *)&caddr, &clen);
    if(fd < 0) {
      if(errno == EINTR || errno == ECONNABORTED) {
        continue;
      }
      LOG_ERR("accept: %s", strerror(errno));
      usleep(100 * 1000);
      continue;
    }

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int bufsz = 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

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
    if(pthread_create(&th, &attr, conn_thread, ca)) {
      close(fd);
      free(ca);
    }
    pthread_attr_destroy(&attr);
  }
}
