/* PS5AIO - LAN discovery.
 *
 * The PC app broadcasts "PS5AIO_DISCOVER" on UDP port 8687; every console
 * running the payload answers with a small JSON description.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "api.h"
#include "discovery.h"
#include "log.h"
#include "plat.h"

#define DISCOVER_MAGIC "PS5AIO_DISCOVER"

static void *
discovery_thread(void *p) {
  struct sockaddr_in addr;
  int one = 1;
  int fd;
  (void)p;

  if((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    LOG_ERR("discovery socket: %s", strerror(errno));
    return NULL;
  }
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(PS5AIO_DISC_PORT);
  if(bind(fd, (struct sockaddr *)&addr, sizeof(addr))) {
    LOG_ERR("discovery bind(%d): %s", PS5AIO_DISC_PORT, strerror(errno));
    close(fd);
    return NULL;
  }
  LOG_INFO("discovery listening on UDP %d", PS5AIO_DISC_PORT);

  for(;;) {
    char buf[256];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &flen);
    if(n <= 0) {
      if(n < 0 && errno != EINTR) {
        usleep(100 * 1000);
      }
      continue;
    }
    buf[n] = 0;
    if(strncmp(buf, DISCOVER_MAGIC, strlen(DISCOVER_MAGIC))) {
      continue;
    }

    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_kstr(&w, "name", PS5AIO_NAME);
    jw_kstr(&w, "version", PS5AIO_VERSION);
    jw_kint(&w, "port", PS5AIO_HTTP_PORT);
    jw_kstr(&w, "platform", plat_name());
    jw_obj_end(&w);
    if(w.data) {
      sendto(fd, w.data, w.len, 0, (struct sockaddr *)&from, flen);
    }
    jw_free(&w);
  }
  return NULL;
}

void
discovery_start(void) {
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, 128 * 1024);
  pthread_create(&th, &attr, discovery_thread, NULL);
  pthread_attr_destroy(&attr);
}
