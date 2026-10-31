/* PS5AIO - all-in-one PS5 utility payload.
 *
 * Exposes a REST API (HTTP, port 8686) used by the PS5AIO desktop app and by
 * the built-in web interface, plus UDP discovery on port 8687.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "api.h"
#include "discovery.h"
#include "fsutil.h"
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

/* NeoHEN: a crash leaves one line in the log file, the only trace of it. */
static void
crash(int sig, siginfo_t *si, void *uc) {
  char line[96];
  int fd = log_fd();
  (void)uc;
  if(fd >= 0) {
    int n = snprintf(line, sizeof(line), "crash: signal %d (code %d) at %p\n",
                     sig, si ? si->si_code : 0, si ? si->si_addr : NULL);
    if(n > 0) {
      ssize_t w = write(fd, line, (size_t)n);
      (void)w;
    }
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

static void
crash_handlers(void) {
  static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigemptyset(&sa.sa_mask);
  for(size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
    sigaction(sigs[i], &sa, NULL);
  }
}

int
main(int argc, char *argv[]) {
  char logpath[256];
  int port = PS5AIO_HTTP_PORT;

  if(argc > 1) {
    port = atoi(argv[1]);
  }

  plat_init();
  plat_path(PS5AIO_DATA_DIR, logpath, sizeof(logpath));
  fs_mkdirs(logpath, 0777);
  plat_path(PS5AIO_DATA_DIR "/neoweb.log", logpath, sizeof(logpath));
  log_open_file(logpath);
  crash_handlers();
  LOG_INFO("%s %s starting (%s)", PS5AIO_NAME, PS5AIO_VERSION, plat_name());
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
