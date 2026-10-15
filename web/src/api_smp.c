/* NeoWEB - a door to ShadowMount+'s own HTTP/JSON service.
 *
 * ShadowMount+ answers on the console itself (127.0.0.1:10101 unless its
 * config.ini says otherwise), where nothing outside can reach it. NeoWEB
 * runs on the console, so it can ask on behalf of the page:
 *
 *   GET  /api/smp?path=/api/v1/...     passed on as a GET
 *   POST /api/smp?path=/api/v1/...     passed on with the body it came with
 *
 * Only paths under /api/ are passed on, and the answer comes back as it is:
 * status, type and body.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "fsutil.h"
#include "plat.h"

#define SMP_CONFIG  "/data/shadowmount/config.ini"
#define SMP_PORT    10101
#define SMP_MAX     (8 << 20)
#define SMP_TIMEOUT 20

/* api_port= in ShadowMount+'s config.ini, when the user moved it */
static int
smp_port(void) {
  const char *env = getenv("NEOWEB_SMP_PORT"); /* the tests' stand-in */
  char local[PATH_MAX];
  char *data = NULL;
  size_t len = 0;
  int port = SMP_PORT;

  if(env && atoi(env) > 0) {
    return atoi(env);
  }
  plat_path(SMP_CONFIG, local, sizeof(local));
  if(!fs_read_file(local, &data, &len, 1 << 20)) {
    for(char *line = data; line; line = strchr(line, '\n')) {
      line += *line == '\n';
      if(!strncmp(line, "api_port=", 9) && atoi(line + 9) > 0 && atoi(line + 9) < 65536) {
        port = atoi(line + 9);
      }
    }
    free(data);
  }
  return port;
}

static int
send_all(int fd, const char *p, size_t len) {
  while(len) {
    ssize_t n = send(fd, p, len, 0);
    if(n < 0 && errno == EINTR) {
      continue;
    }
    if(n <= 0) {
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

/* One request to ShadowMount+. On success *reply holds the whole HTTP
 * answer (NUL-terminated, free it), *body points at its body and the HTTP
 * status is returned; -1 if it did not answer, with why in err. */
static int
smp_exchange(const char *method, const char *path, const char *body, size_t blen,
             char **reply_out, char **body_out, size_t *body_len, char *err, size_t esize) {
  struct timeval tv = {SMP_TIMEOUT, 0};
  struct sockaddr_in addr;
  char head[2400], *reply = NULL, *sep;
  size_t rlen = 0, cap = 65536;
  int fd, status = 0;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)smp_port());
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0 ||
     setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) ||
     connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
    if(fd >= 0) {
      close(fd);
    }
    snprintf(err, esize, "ShadowMount+ is not answering on port %d", smp_port());
    return -1;
  }
  snprintf(head, sizeof(head),
           "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"
           "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
           method, path, blen);
  if(send_all(fd, head, strlen(head)) || (blen && send_all(fd, body, blen)) ||
     !(reply = malloc(cap))) {
    close(fd);
    free(reply);
    snprintf(err, esize, "ShadowMount+ did not take the request");
    return -1;
  }
  for(;;) {
    ssize_t n;
    if(rlen + 1 >= cap) {
      char *more = cap < SMP_MAX ? realloc(reply, cap * 2) : NULL;
      if(!more) {
        break;
      }
      reply = more;
      cap *= 2;
    }
    n = recv(fd, reply + rlen, cap - 1 - rlen, 0);
    if(n < 0 && errno == EINTR) {
      continue;
    }
    if(n <= 0) {
      break;
    }
    rlen += (size_t)n;
  }
  close(fd);
  reply[rlen] = 0;
  if(sscanf(reply, "HTTP/%*s %d", &status) != 1 || !(sep = strstr(reply, "\r\n\r\n"))) {
    free(reply);
    snprintf(err, esize, "ShadowMount+ gave an answer that is not HTTP");
    return -1;
  }
  *sep = 0;
  *reply_out = reply;
  *body_out = sep + 4;
  *body_len = rlen - (size_t)(sep + 4 - reply);
  return status;
}

int
smp_call(const char *path, const char *json_body, jval_t **out, char *err, size_t esize) {
  char *reply = NULL, *body = NULL;
  size_t len = 0;
  int status;
  jval_t *v;

  if(out) {
    *out = NULL;
  }
  status = smp_exchange("POST", path, json_body ? json_body : "{}",
                        strlen(json_body ? json_body : "{}"), &reply, &body, &len, err, esize);
  if(status < 0) {
    return -1;
  }
  v = json_parse(body, len);
  free(reply);
  if(!v) {
    snprintf(err, esize, "ShadowMount+ answered %d with no JSON", status);
    return -1;
  }
  /* its API says how it went in "status": 0 is fine */
  if(status != 200 || json_get_int(v, "status", 0)) {
    snprintf(err, esize, "%s", json_get_str(v, "error", "ShadowMount+ said no"));
    json_free(v);
    return -1;
  }
  if(out) {
    *out = v;
  } else {
    json_free(v);
  }
  return 0;
}

void
api_smp(http_req_t *req) {
  char path[2048], ctype[128] = "application/json", err[160];
  char *body = NULL, *reply = NULL, *rbody, *line;
  size_t blen = 0, rblen = 0;
  int status, post = !strcmp(req->method, "POST");

  if(!http_query(req, "path", path, sizeof(path)) || strncmp(path, "/api/", 5) ||
     strpbrk(path, " \r\n") || strstr(path, "..")) {
    http_send_error(req, 400, "path must be under /api/");
    return;
  }
  if(post && !(body = http_read_body_all(req, 1 << 20, &blen))) {
    http_send_error(req, 400, "the body could not be read");
    return;
  }
  status = smp_exchange(post ? "POST" : "GET", path, body, blen, &reply, &rbody, &rblen,
                        err, sizeof(err));
  free(body);
  if(status < 0) {
    http_send_error(req, 502, "%s", err);
    return;
  }
  for(line = strstr(reply, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
    if(!strncasecmp(line + 2, "Content-Type:", 13)) {
      sscanf(line + 15, " %127[^\r\n]", ctype);
    }
  }
  http_send_buf(req, status, ctype, rbody, rblen);
  free(reply);
}
