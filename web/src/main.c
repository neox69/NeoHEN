/* PS5AIO - all-in-one PS5 utility payload.
 *
 * Exposes a REST API (HTTP, port 8686) used by the PS5AIO desktop app and by
 * the built-in web interface, plus UDP discovery on port 8687.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>

#include "api.h"
#include "discovery.h"
#include "http.h"
#include "log.h"
#include "plat.h"

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

  plat_notify(PS5AIO_NAME " %s: port %d", PS5AIO_VERSION, port);

  if(http_serve((uint16_t)port, api_dispatch)) {
    plat_notify(PS5AIO_NAME ": could not listen on port %d", port);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
