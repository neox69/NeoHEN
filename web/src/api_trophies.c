/* NeoWEB - trophies, as the console itself keeps count of them.
 *
 * Each user has a small SQLite file the menu fills in:
 *
 *   /user/home/<user>/trophy_ui/trophy_tracker.db
 *     titles          a set of trophies: how many of each grade, name, icon
 *     title_records   how many of each the user has, progress, last date
 *     id_bindings     which games (PPSA01234_00) a set belongs to
 *
 * This reads the three and joins them. Nothing is written, and nothing is
 * asked of the network: the icon is the address the console already had.
 * The names of the single trophies are not in this file.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "sqlite_ro.h"

#define HOME "/user/home"
#define MAX_SETS 4096

typedef struct set {
  char id[40]; /* 20:NPWR22859_00 */
  char name[160];
  char icon[320];
  char updated[40];
  char games[200]; /* PPSA03208 CUSA07439, space separated */
  int total[4], got[4]; /* platinum, gold, silver, bronze */
  int progress;
  int has_record;
} set_t;

typedef struct sets {
  set_t *v;
  size_t n;
} sets_t;

static void
text_of(const sq_val_t *v, char *out, size_t size) {
  size_t n = v->type == SQ_TEXT ? v->len : 0;

  if(n >= size) {
    n = size - 1;
  }
  if(n) {
    memcpy(out, v->text, n);
  }
  out[n] = 0;
}

static int
int_of(const sq_val_t *v) {
  return v->type == SQ_INT ? (int)v->i : 0;
}

static set_t *
set_for(sets_t *s, const char *id, int make) {
  for(size_t i = 0; i < s->n; i++) {
    if(!strcmp(s->v[i].id, id)) {
      return &s->v[i];
    }
  }
  if(!make || s->n == MAX_SETS || !id[0]) {
    return NULL;
  }
  set_t *t = &s->v[s->n++];
  memset(t, 0, sizeof(*t));
  snprintf(t->id, sizeof(t->id), "%s", id);
  return t;
}

/* titles: titleid, platform_set, platinum, gold, silver, bronze, has_group, name, icon_uri */
static int
row_title(int64_t rowid, const sq_val_t *v, int n, void *arg) {
  char id[40];
  set_t *t;

  (void)rowid;
  if(n < 9) {
    return 0;
  }
  text_of(&v[0], id, sizeof(id));
  if((t = set_for(arg, id, 1))) {
    for(int i = 0; i < 4; i++) {
      t->total[i] = int_of(&v[2 + i]);
    }
    text_of(&v[7], t->name, sizeof(t->name));
    text_of(&v[8], t->icon, sizeof(t->icon));
  }
  return 0;
}

/* title_records: titleid, unlocked platinum/gold/silver/bronze, progress, time_last_update */
static int
row_record(int64_t rowid, const sq_val_t *v, int n, void *arg) {
  char id[40];
  set_t *t;

  (void)rowid;
  if(n < 7) {
    return 0;
  }
  text_of(&v[0], id, sizeof(id));
  if((t = set_for(arg, id, 0))) {
    for(int i = 0; i < 4; i++) {
      t->got[i] = int_of(&v[1 + i]);
    }
    t->progress = int_of(&v[5]);
    text_of(&v[6], t->updated, sizeof(t->updated));
    t->has_record = 1;
  }
  return 0;
}

/* id_bindings: nptitleid (PPSA03208_00), titleid */
static int
row_binding(int64_t rowid, const sq_val_t *v, int n, void *arg) {
  char game[40], id[40];
  set_t *t;

  (void)rowid;
  if(n < 2) {
    return 0;
  }
  text_of(&v[0], game, sizeof(game));
  text_of(&v[1], id, sizeof(id));
  game[9] = 0; /* the title id without its _00 */
  if(is_title_id(game) && (t = set_for(arg, id, 0)) &&
     strlen(t->games) + 11 < sizeof(t->games) && !strstr(t->games, game)) {
    if(t->games[0]) {
      strcat(t->games, " ");
    }
    strcat(t->games, game);
  }
  return 0;
}

static int
is_user_id(const char *s) {
  if(strlen(s) != 8) {
    return 0;
  }
  for(int i = 0; i < 8; i++) {
    if(!isxdigit((unsigned char)s[i])) {
      return 0;
    }
  }
  return 1;
}

static void
user_name(const char *user, char *out, size_t size) {
  char path[PATH_MAX], local[PATH_MAX], raw[32] = "";
  size_t n = 0;
  FILE *f;

  snprintf(path, sizeof(path), HOME "/%s/username.dat", user);
  plat_path(path, local, sizeof(local));
  if((f = fopen(local, "rb"))) {
    n = fread(raw, 1, sizeof(raw) - 1, f);
    fclose(f);
  }
  raw[n] = 0;
  snprintf(out, size, "%s", raw[0] ? raw : user);
}

/* GET /api/trophies */
void
api_trophies(http_req_t *req) {
  static const char *const GRADES[] = {"platinum", "gold", "silver", "bronze"};
  char local[PATH_MAX], path[PATH_MAX];
  struct dirent *u;
  sets_t s = {0};
  DIR *home;
  jw_t w;

  if(!(s.v = malloc(MAX_SETS * sizeof(set_t)))) {
    http_send_error(req, 500, "out of memory");
    return;
  }
  api_begin(&w);
  jw_key(&w, "users");
  jw_arr(&w);
  plat_path(HOME, local, sizeof(local));
  if((home = opendir(local))) {
    while((u = readdir(home))) {
      char name[40];
      sq_db_t *db;

      if(!is_user_id(u->d_name)) {
        continue;
      }
      snprintf(path, sizeof(path), HOME "/%s/trophy_ui/trophy_tracker.db", u->d_name);
      plat_path(path, local, sizeof(local));
      if(!(db = sq_open(local))) {
        continue;
      }
      s.n = 0;
      sq_table(db, "titles", row_title, &s);
      sq_table(db, "title_records", row_record, &s);
      sq_table(db, "id_bindings", row_binding, &s);
      sq_close(db);
      if(!s.n) {
        continue;
      }
      user_name(u->d_name, name, sizeof(name));
      jw_obj(&w);
      jw_kstr(&w, "id", u->d_name);
      jw_kstr(&w, "name", name);
      jw_key(&w, "sets");
      jw_arr(&w);
      for(size_t i = 0; i < s.n; i++) {
        const set_t *t = &s.v[i];
        const char *colon = strchr(t->id, ':');

        jw_obj(&w);
        jw_kstr(&w, "id", colon ? colon + 1 : t->id);
        jw_kstr(&w, "platform", !strncmp(t->id, "10:", 3) ? "ps4" : "ps5");
        jw_kstr(&w, "name", t->name);
        jw_kstr(&w, "icon", t->icon);
        jw_kstr(&w, "games", t->games);
        jw_kint(&w, "progress", t->progress);
        jw_kstr(&w, "updated", t->updated);
        jw_key(&w, "total");
        jw_obj(&w);
        for(int g = 0; g < 4; g++) {
          jw_kint(&w, GRADES[g], t->total[g]);
        }
        jw_obj_end(&w);
        jw_key(&w, "got");
        jw_obj(&w);
        for(int g = 0; g < 4; g++) {
          jw_kint(&w, GRADES[g], t->got[g]);
        }
        jw_obj_end(&w);
        jw_obj_end(&w);
      }
      jw_arr_end(&w);
      jw_obj_end(&w);
    }
    closedir(home);
  }
  jw_arr_end(&w);
  free(s.v);
  api_finish(req, &w);
}
