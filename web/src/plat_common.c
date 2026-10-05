/* PS5AIO - platform helpers shared by every backend.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "plat.h"

static pthread_key_t  g_err_key;
static pthread_once_t g_err_once = PTHREAD_ONCE_INIT;

static void
err_key_init(void) {
  pthread_key_create(&g_err_key, free);
}

static char *
err_buf(void) {
  pthread_once(&g_err_once, err_key_init);
  char *b = pthread_getspecific(g_err_key);
  if(!b && (b = calloc(1, 512))) {
    pthread_setspecific(g_err_key, b);
  }
  return b;
}

const char *
plat_error(void) {
  char *b = err_buf();
  return b && b[0] ? b : "unknown error";
}

void
plat_set_error(const char *fmt, ...) {
  char *b = err_buf();
  va_list ap;
  if(!b) {
    return;
  }
  va_start(ap, fmt);
  vsnprintf(b, 512, fmt, ap);
  va_end(ap);
}
