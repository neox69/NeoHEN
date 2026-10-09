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

void
api_smp(http_req_t *req) {
  struct timeval tv = {SMP_TIMEOUT, 0};
  char path[2048], head[2400], ctype[128] = "application/json";
  struct sockaddr_in addr;
  char *body = NULL, *reply = NULL, *sep, *line;
  size_t blen = 0, rlen = 0, cap = 65536;
  int fd, status = 0, post = !strcmp(req->method, "POST");

  if(!http_query(req, "path", path, sizeof(path)) || strncmp(path, "/api/", 5) ||
     strpbrk(path, " \r\n") || strstr(path, "..")) {
    http_send_error(req, 400, "path must be under /api/");
    return;
  }
  if(post && !(body = http_read_body_all(req, 1 << 20, &blen))) {
    http_send_error(req, 400, "the body could not be read");
    return;
  }
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
    free(body);
    http_send_error(req, 502, "ShadowMount+ is not answering on port %d", smp_port());
    return;
  }
  snprintf(head, sizeof(head),
           "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"
           "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
           post ? "POST" : "GET", path, blen);
  if(send_all(fd, head, strlen(head)) || (blen && send_all(fd, body, blen)) ||
     !(reply = malloc(cap))) {
    close(fd);
    free(body);
    free(reply);
    http_send_error(req, 502, "ShadowMount+ did not take the request");
    return;
  }
  free(body);
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
    http_send_error(req, 502, "ShadowMount+ gave an answer that is not HTTP");
    return;
  }
  *sep = 0;
  for(line = strstr(reply, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
    if(!strncasecmp(line + 2, "Content-Type:", 13)) {
      sscanf(line + 15, " %127[^\r\n]", ctype);
    }
  }
  sep += 4;
  http_send_buf(req, status, ctype, sep, rlen - (size_t)(sep - reply));
  free(reply);
}
