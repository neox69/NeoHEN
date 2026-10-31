/* PS5AIO - logging (stdout + in-memory ring buffer readable over the API,
 * and NeoHEN: a file that outlives the process, see log_open_file()).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "log.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

/* stdout of a payload goes nowhere anyone can read, so when NeoWEB died in
 * Rest Mode nothing said why. The file keeps the last two halves. */
#define LOG_FILE_MAX (512 * 1024)
static int  g_fd = -1;
static char g_path[256];

int
log_fd(void) {
  return g_fd;
}

static void
log_file_rotate(void) {
  char old[sizeof(g_path) + 2];
  struct stat st;
  if(g_fd < 0 || fstat(g_fd, &st) || st.st_size < LOG_FILE_MAX) {
    return;
  }
  snprintf(old, sizeof(old), "%s.1", g_path);
  close(g_fd);
  rename(g_path, old);
  g_fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND, 0666);
}

void
log_open_file(const char *path) {
  snprintf(g_path, sizeof(g_path), "%s", path);
  pthread_mutex_lock(&g_lock);
  g_fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND, 0666);
  log_file_rotate();
  pthread_mutex_unlock(&g_lock);
}

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
  if(g_fd >= 0) {
    char line[320];
    struct tm tm;
    localtime_r(&e->ts, &tm);
    int n = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d %s: %s\n",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                     tm.tm_min, tm.tm_sec, g_names[level & 3], msg);
    if(n > 0) {
      ssize_t w = write(g_fd, line, n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1);
      (void)w;
    }
    log_file_rotate();
  }
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
