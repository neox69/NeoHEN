/* PS5AIO - logging (stdout + in-memory ring buffer readable over the API).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define LOG_RING 256

typedef struct log_entry {
  unsigned seq;
  int      level;
  time_t   ts;
  char     msg[256];
} log_entry_t;

static log_entry_t     g_ring[LOG_RING];
static unsigned        g_seq;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *g_names[] = {"debug", "info", "warn", "error"};

void
log_write(int level, const char *fmt, ...) {
  char msg[256];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  printf("[neoweb] %s: %s\n", g_names[level & 3], msg);
  fflush(stdout);

  pthread_mutex_lock(&g_lock);
  log_entry_t *e = &g_ring[g_seq % LOG_RING];
  e->seq = ++g_seq;
  e->level = level;
  e->ts = time(NULL);
  snprintf(e->msg, sizeof(e->msg), "%s", msg);
  pthread_mutex_unlock(&g_lock);
}

void
log_dump_json(jw_t *w, unsigned since) {
  pthread_mutex_lock(&g_lock);
  unsigned first = g_seq > LOG_RING ? g_seq - LOG_RING + 1 : 1;
  if(since + 1 > first) {
    first = since + 1;
  }
  jw_arr(w);
  for(unsigned s = first; s <= g_seq; s++) {
    log_entry_t *e = &g_ring[(s - 1) % LOG_RING];
    jw_obj(w);
    jw_kuint(w, "seq", e->seq);
    jw_kstr(w, "level", g_names[e->level & 3]);
    jw_kint(w, "ts", (int64_t)e->ts);
    jw_kstr(w, "msg", e->msg);
    jw_obj_end(w);
  }
  jw_arr_end(w);
  pthread_mutex_unlock(&g_lock);
}
