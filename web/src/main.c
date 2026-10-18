/* PS5AIO - all-in-one PS5 utility payload.
 *
 * Exposes a REST API (HTTP, port 8686) used by the PS5AIO desktop app and by
 * the built-in web interface, plus UDP discovery on port 8687.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include "api.h"
#include "discovery.h"
#include "http.h"
#include "log.h"
#include "plat.h"

/* The address to type on a phone or a PC, said on the TV as soon as the
 * console has one (at boot the network can take a while). */
static int g_port;

static void *
welcome(void *arg) {
  char ip[64];
  (void)arg;
  for(int i = 0; i < 60; i++) {
    if(!plat_first_ip(ip, sizeof(ip))) {
      plat_notify(PS5AIO_NAME " %s\nhttp://%s:%d", PS5AIO_VERSION, ip, g_port);
      return NULL;
    }
    sleep(1);
  }
  plat_notify(PS5AIO_NAME " %s: port %d", PS5AIO_VERSION, g_port);
  return NULL;
}

static void
welcome_start(int port) {
  pthread_t t;
  g_port = port;
  if(pthread_create(&t, NULL, welcome, NULL)) {
    plat_notify(PS5AIO_NAME " %s: port %d", PS5AIO_VERSION, port);
    return;
  }
  pthread_detach(t);
}

int
main(int argc, char *argv[]) {
  int port = PS5AIO_HTTP_PORT;

  if(argc > 1) {
    port = atoi(argv[1]);
  }

  LOG_INFO("%s %s starting (%s)", PS5AIO_NAME, PS5AIO_VERSION, plat_name());
  plat_init();
  api_init();
#ifdef NEOWEB_FULL
  discovery_start();
#endif

  welcome_start(port);

  if(http_serve((uint16_t)port, api_dispatch)) {
    plat_notify(PS5AIO_NAME ": could not listen on port %d", port);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
