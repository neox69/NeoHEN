/* NeoWEB - play time, and what the user says about each game.
 *
 * The console does not hand out how long each game has been played, so this
 * counts it: a thread looks every few seconds at which game is open and adds
 * the time to that game, by day. It only counts while NeoWEB is running.
 *
 *   /data/neohen/web/playtime.json   {"games": {"PPSA01234": {"total": 5400,
 *                                     "sessions": 3, "first": .., "last": ..,
 *                                     "days": {"2026-10-08": 3600, ...}}}}
 *   /data/neohen/web/library.json    {"status": {"PPSA01234": "playing"}}
 *
 * The status is the user's own note: pending, playing, done or dropped.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fsutil.h"
#include "log.h"
#include "plat.h"

#define PLAYTIME PS5AIO_DATA_DIR "/playtime.json"
#define LIBRARY  PS5AIO_DATA_DIR "/library.json"
#define MAX_GAMES 1024
#define SAVE_EVERY_S 60
#define KEEP_DAYS 800 /* per game; older days stay in the total */

typedef struct day {
  char date[12]; /* 2026-10-08 */
  uint32_t seconds;
} day_t;

typedef struct game {
  char id[12];
  uint64_t total;
  uint32_t sessions;
  int64_t first, last;
  day_t *days;
  size_t ndays, cap;
  char status[12];
} game_t;

static game_t g_games[MAX_GAMES];
static size_t g_ngames;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_dirty;

static game_t *
game_for(const char *id, int make) {
  for(size_t i = 0; i < g_ngames; i++) {
    if(!strcmp(g_games[i].id, id)) {
      return &g_games[i];
    }
  }
  if(!make || g_ngames == MAX_GAMES || !is_title_id(id)) {
    return NULL;
  }
  game_t *g = &g_games[g_ngames++];
  memset(g, 0, sizeof(*g));
  snprintf(g->id, sizeof(g->id), "%s", id);
  return g;
}

static day_t *
day_for(game_t *g, const char *date) {
  for(size_t i = g->ndays; i-- > 0;) {
    if(!strcmp(g->days[i].date, date)) {
      return &g->days[i];
    }
  }
  if(g->ndays == g->cap) {
    size_t cap = g->cap ? g->cap * 2 : 16;
    day_t *d = realloc(g->days, cap * sizeof(day_t));
    if(!d) {
      return NULL;
    }
    g->days = d;
    g->cap = cap;
  }
  if(g->ndays >= KEEP_DAYS) {
    memmove(g->days, g->days + 1, (g->ndays - 1) * sizeof(day_t));
    g->ndays--;
  }
  day_t *d = &g->days[g->ndays++];
  memset(d, 0, sizeof(*d));
  snprintf(d->date, sizeof(d->date), "%s", date);
  return d;
}

static void
today(char *out, size_t size) {
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  strftime(out, size, "%Y-%m-%d", &tm);
}

/* ------------------------------------------------------------ the files */

static void
load_file(const char *console, int statuses) {
  char local[PATH_MAX];
  char *text = NULL;
  size_t len = 0;
  jval_t *root, *set;

  plat_path(console, local, sizeof(local));
  if(fs_read_file(local, &text, &len, 8 << 20)) {
    return;
  }
  root = json_parse(text, len);
  free(text);
  if(!root) {
    LOG_WARN("%s cannot be read as JSON: left alone, starting empty", console);
    return;
  }
  set = json_get(root, statuses ? "status" : "games");
  for(jval_t *v = set ? set->child : NULL; v; v = v->next) {
    game_t *g = v->key ? game_for(v->key, 1) : NULL;
    if(!g) {
      continue;
    }
    if(statuses) {
      if(v->type == J_STR) {
        snprintf(g->status, sizeof(g->status), "%s", v->str);
      }
      continue;
    }
    g->total = json_get_u64(v, "total", 0);
    g->sessions = (uint32_t)json_get_u64(v, "sessions", 0);
    g->first = json_get_int(v, "first", 0);
    g->last = json_get_int(v, "last", 0);
    jval_t *days = json_get(v, "days");
    for(jval_t *d = days ? days->child : NULL; d; d = d->next) {
      day_t *day = d->key && strlen(d->key) == 10 ? day_for(g, d->key) : NULL;
      if(day && d->type == J_NUM) {
        day->seconds = (uint32_t)d->num;
      }
    }
  }
  json_free(root);
}

static void
write_games(jw_t *w) {
  jw_key(w, "games");
  jw_obj(w);
  for(size_t i = 0; i < g_ngames; i++) {
    const game_t *g = &g_games[i];
    if(!g->total && !g->sessions) {
      continue;
    }
    jw_key(w, g->id);
    jw_obj(w);
    jw_kuint(w, "total", g->total);
    jw_kuint(w, "sessions", g->sessions);
    jw_kint(w, "first", g->first);
    jw_kint(w, "last", g->last);
    jw_key(w, "days");
    jw_obj(w);
    for(size_t d = 0; d < g->ndays; d++) {
      jw_kuint(w, g->days[d].date, g->days[d].seconds);
    }
    jw_obj_end(w);
    jw_obj_end(w);
  }
  jw_obj_end(w);
}

static void
write_statuses(jw_t *w) {
  jw_key(w, "status");
  jw_obj(w);
  for(size_t i = 0; i < g_ngames; i++) {
    if(g_games[i].status[0]) {
      jw_kstr(w, g_games[i].id, g_games[i].status);
    }
  }
  jw_obj_end(w);
}

/* Written whole next to the file and put in its place: never half a file. */
static void
save_file(const char *console, void (*body)(jw_t *)) {
  char local[PATH_MAX], tmp[PATH_MAX];
  jw_t w;

  jw_init(&w);
  jw_obj(&w);
  jw_kint(&w, "version", 1);
  body(&w);
  jw_obj_end(&w);
  plat_path(console, local, sizeof(local));
  snprintf(tmp, sizeof(tmp), "%s.new", local);
  if(!w.oom && !fs_write_file(tmp, w.data, w.len)) {
    rename(tmp, local);
  }
  jw_free(&w);
}

/* ------------------------------------------------------------- counting */

static void *
count_thread(void *arg) {
  const char *env = getenv("NEOWEB_PLAY_TICK_MS"); /* the tests go faster */
  long tick_ms = env && atol(env) > 0 ? atol(env) : 15000;
  char open_now[16] = "";
  struct timespec before, now;
  long carry_ms = 0, unsaved_ms = 0;

  (void)arg;
  clock_gettime(CLOCK_MONOTONIC, &before);
  for(;;) {
    char tid[16] = "", date[12];
    int app_id = 0;
    pid_t pid = -1;
    long ms;

    usleep((useconds_t)tick_ms * 1000);
    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (now.tv_sec - before.tv_sec) * 1000L + (now.tv_nsec - before.tv_nsec) / 1000000L;
    before = now;
    /* a jump (Rest Mode, a stalled clock) is not play time */
    if(ms < 0 || ms > tick_ms * 4) {
      ms = 0;
    }
    if(plat_app_running(tid, sizeof(tid), &app_id, &pid) || !is_title_id(tid) ||
       !strncmp(tid, "NPXS", 4)) {
      tid[0] = 0;
    }
    pthread_mutex_lock(&g_lock);
    if(tid[0]) {
      game_t *g = game_for(tid, 1);
      if(g) {
        int64_t stamp = (int64_t)time(NULL);
        long whole;
        if(strcmp(open_now, tid)) {
          g->sessions++;
          carry_ms = 0;
        }
        carry_ms += ms;
        whole = carry_ms / 1000;
        carry_ms -= whole * 1000;
        today(date, sizeof(date));
        day_t *d = day_for(g, date);
        g->total += (uint64_t)whole;
        if(d) {
          d->seconds += (uint32_t)whole;
        }
        if(!g->first) {
          g->first = stamp;
        }
        g->last = stamp;
        g_dirty = 1;
      }
    }
    snprintf(open_now, sizeof(open_now), "%s", tid);
    unsaved_ms += tick_ms;
    if(g_dirty && (unsaved_ms >= SAVE_EVERY_S * 1000L || !tid[0])) {
      save_file(PLAYTIME, write_games);
      g_dirty = 0;
      unsaved_ms = 0;
    }
    pthread_mutex_unlock(&g_lock);
  }
  return NULL;
}

void
api_stats_init(void) {
  pthread_t thread;

  pthread_mutex_lock(&g_lock);
  load_file(PLAYTIME, 0);
  load_file(LIBRARY, 1);
  pthread_mutex_unlock(&g_lock);
  if(!pthread_create(&thread, NULL, count_thread, NULL)) {
    pthread_detach(thread);
  }
}

/* ------------------------------------------------------------------ API */

/* GET /api/stats/playtime */
void
api_stats_playtime(http_req_t *req) {
  char date[12];
  jw_t w;

  today(date, sizeof(date));
  api_begin(&w);
  jw_kstr(&w, "today", date);
  pthread_mutex_lock(&g_lock);
  write_games(&w);
  pthread_mutex_unlock(&g_lock);
  api_finish(req, &w);
}

/* GET /api/library/status */
void
api_library_status_get(http_req_t *req) {
  jw_t w;

  api_begin(&w);
  pthread_mutex_lock(&g_lock);
  write_statuses(&w);
  pthread_mutex_unlock(&g_lock);
  api_finish(req, &w);
}

/* POST /api/library/status {"titleId":"PPSA01234","status":"playing"}
 * status: pending | playing | done | dropped | "" (none) */
void
api_library_status_set(http_req_t *req) {
  static const char *const VALID[] = {"", "pending", "playing", "done", "dropped"};
  char id[16], status[16];
  int ok = 0;
  jval_t *b = api_body(req);

  if(!b) {
    return;
  }
  snprintf(id, sizeof(id), "%s", json_get_str(b, "titleId", ""));
  snprintf(status, sizeof(status), "%s", json_get_str(b, "status", ""));
  json_free(b);
  for(size_t i = 0; i < sizeof(VALID) / sizeof(VALID[0]); i++) {
    ok |= !strcmp(status, VALID[i]);
  }
  if(!is_title_id(id) || !ok) {
    http_send_error(req, 400, "a title id and one of: pending, playing, done, dropped");
    return;
  }
  pthread_mutex_lock(&g_lock);
  game_t *g = game_for(id, 1);
  if(g) {
    snprintf(g->status, sizeof(g->status), "%s", status);
    save_file(LIBRARY, write_statuses);
  }
  pthread_mutex_unlock(&g_lock);
  if(!g) {
    http_send_error(req, 507, "too many games");
    return;
  }
  http_send_ok(req);
}
