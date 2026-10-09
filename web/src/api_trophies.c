/* NeoWEB - trophies, as the console itself keeps count of them.
 *
 * Each user has a small SQLite file the menu fills in:
 *
 *   /user/home/<user>/trophy_ui/trophy_tracker.db
 *     titles          a set of trophies: how many of each grade, name, icon
 *     title_records   how many of each the user has, progress, last date
 *     id_bindings     which games (PPSA01234_00) a set belongs to
 *
 * That file is only brought up to date when the console talks to PlayStation
 * Network. What is earned without it lives next to it, one folder per set:
 *
 *   /user/home/<user>/trophy2/nobackup/data/<set>/TRPTITLE.DAT
 *     which trophies this user has, and when (see dat_read)
 *   /user/trophy2/nobackup/conf/<set>/TROPHY.UCP
 *     the set as the game installed it: an archive with tropconf.json (grade
 *     of each trophy), tropmeta_<language>.json (names) and the icons
 *
 * This reads both and joins them; where they disagree, the one with more
 * trophies wins. Nothing is written, and nothing is asked of the network.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fsutil.h"
#include "plat.h"
#include "sqlite_ro.h"

#define HOME "/user/home"
#define CONF "/user/trophy2/nobackup/conf"
#define UCP_MAGIC 0xb228c60au
#define TICKS_TO_UNIX 62135596800ll /* seconds from year 1 to 1970 */
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
  int local;      /* counted from the console's own files */
  int local_icon; /* the set's icon can be served from TROPHY.UCP */
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

/* ---------------------------------------------------- the console's own files */

typedef struct unlock {
  int id;
  int got;
  int64_t when; /* unix seconds, 0 when unknown */
} unlock_t;

static uint32_t
be32(const unsigned char *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t
be64(const unsigned char *p) {
  return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static int
is_set_id(const char *s) {
  /* NPWR12345_00 */
  if(strlen(s) != 12 || strncmp(s, "NPWR", 4) || s[9] != '_') {
    return 0;
  }
  for(int i = 4; i < 12; i++) {
    if(i != 9 && !isdigit((unsigned char)s[i])) {
      return 0;
    }
  }
  return 1;
}

/* TROPHY.UCP: a 0x60 byte header, then one 64 byte entry per file (name,
 * offset, size), then the files as they are. */
static int
ucp_find(FILE *f, const char *name, uint64_t *off, uint64_t *size) {
  unsigned char h[0x60], e[0x40];
  uint32_t n;

  if(fseek(f, 0, SEEK_SET) || fread(h, 1, sizeof(h), f) != sizeof(h) ||
     be32(h) != UCP_MAGIC || be32(h + 0x14) != sizeof(e)) {
    return -1;
  }
  n = be32(h + 0x10);
  for(uint32_t i = 0; i < n && i < 8192; i++) {
    if(fread(e, 1, sizeof(e), f) != sizeof(e)) {
      return -1;
    }
    if(!strncmp((const char *)e, name, 32)) {
      *off = be64(e + 32);
      *size = be64(e + 40);
      return 0;
    }
  }
  return -1;
}

static FILE *
ucp_open(const char *set) {
  char path[PATH_MAX], local[PATH_MAX];

  snprintf(path, sizeof(path), CONF "/%s/TROPHY.UCP", set);
  plat_path(path, local, sizeof(local));
  return fopen(local, "rb");
}

/* one file out of the archive, NUL terminated; the caller frees it */
static char *
ucp_read(FILE *f, const char *name, size_t max, size_t *len) {
  uint64_t off, size;
  char *data;

  if(ucp_find(f, name, &off, &size) || size > max || !(data = malloc(size + 1))) {
    return NULL;
  }
  if(fseek(f, (long)off, SEEK_SET) || fread(data, 1, size, f) != size) {
    free(data);
    return NULL;
  }
  data[size] = 0;
  *len = size;
  return data;
}

static jval_t *
ucp_json(FILE *f, const char *name) {
  size_t len;
  char *text = ucp_read(f, name, 4 << 20, &len);
  jval_t *v = text ? json_parse(text, len) : NULL;

  free(text);
  return v;
}

/* the names of a set, in the language asked for or the nearest there is */
static jval_t *
ucp_meta(FILE *f, const jval_t *conf, const char *lang) {
  const char *try[] = {lang, json_get_str(conf, "defaultLanguage", ""), "en-US"};
  char name[48];
  jval_t *v;

  for(size_t i = 0; i < sizeof(try) / sizeof(try[0]); i++) {
    if(try[i][0]) {
      snprintf(name, sizeof(name), "tropmeta_%s.json", try[i]);
      if((v = ucp_json(f, name))) {
        return v;
      }
    }
  }
  return NULL;
}

/* TRPTITLE.DAT: 0x40 bytes of header, then the data in blocks that each end
 * in a 0x20 byte hash. With the hashes taken out it is a table of sections;
 * section 0x800 has one record per trophy: number, earned, and the time as
 * microseconds from year 1. Returns how many records, or -1. */
static int
dat_read(const char *user, const char *set, unlock_t **out) {
  char path[PATH_MAX], local[PATH_MAX];
  unsigned char *raw = NULL, *d;
  size_t len = 0, n = 0, chunk;
  unlock_t *u = NULL;
  int count = -1;

  *out = NULL;
  snprintf(path, sizeof(path), HOME "/%s/trophy2/nobackup/data/%s/TRPTITLE.DAT", user, set);
  plat_path(path, local, sizeof(local));
  if(fs_read_file(local, (char **)&raw, &len, 16 << 20)) {
    return -1;
  }
  if(len < 0x80 || memcmp(raw, "T2PD", 4) || (chunk = be32(raw + 12)) <= 0x20 ||
     !(d = malloc(len))) {
    free(raw);
    return -1;
  }
  for(size_t o = 0x40; o < len; o += chunk) {
    size_t part = chunk - 0x20;
    if(part > len - o) {
      part = len - o;
    }
    memcpy(d + n, raw + o, part);
    n += part;
  }
  free(raw);
  if(n >= 0x30 && !memcmp(d, "T2TD", 4)) {
    uint32_t sections = be32(d + 8);
    for(uint32_t i = 0; i < sections && 0x30 + (i + 1) * 0x20 <= n; i++) {
      const unsigned char *e = d + 0x30 + i * 0x20;
      uint64_t size = (uint64_t)be32(e + 4) + 0x10, records = be32(e + 12), off = be64(e + 16);

      if(be32(e) != 0x800 || size < 0x28 || records > 4096 || off > n ||
         records * size > n - off || !(u = calloc(records ? records : 1, sizeof(*u)))) {
        continue;
      }
      for(uint64_t k = 0; k < records; k++) {
        const unsigned char *r = d + off + k * size + 0x10;
        uint64_t ticks = be64(r + 0x10);

        u[k].id = (int)be32(r);
        u[k].got = be32(r + 4) != 0;
        u[k].when = ticks ? (int64_t)(ticks / 1000000) - TICKS_TO_UNIX : 0;
      }
      count = (int)records;
      break;
    }
  }
  free(d);
  *out = u;
  return count;
}

static const unlock_t *
unlock_of(const unlock_t *u, int n, int id) {
  for(int i = 0; i < n; i++) {
    if(u[i].id == id) {
      return &u[i];
    }
  }
  return NULL;
}

static int
grade_index(const char *g) {
  switch(g[0]) {
  case 'P':
    return 0;
  case 'G':
    return 1;
  case 'S':
    return 2;
  case 'B':
    return 3;
  }
  return -1;
}

static const jval_t *
first_trophy(const jval_t *conf) {
  const jval_t *list = json_get(conf, "trophies");
  return list ? list->child : NULL;
}

static void
iso_time(int64_t when, char *out, size_t size) {
  time_t t = (time_t)when;
  struct tm tm;

  out[0] = 0;
  if(when > 0 && gmtime_r(&t, &tm)) {
    strftime(out, size, "%Y-%m-%dT%H:%M:%SZ", &tm);
  }
}

/* PlayStation's own sum: the platinum does not count, a gold is six bronzes */
static int
progress_of(const int *total, const int *got) {
  static const int W[] = {0, 90, 30, 15};
  int all = 0, have = 0, left = 0;

  for(int g = 0; g < 4; g++) {
    all += total[g] * W[g];
    have += got[g] * W[g];
    left += total[g] - got[g];
  }
  if(left <= 0) {
    return all ? 100 : 0;
  }
  if(!all) {
    return 0;
  }
  have = have * 100 / all;
  return have > 99 ? 99 : have;
}

/* add to the sets of a user what the console counted by itself */
static void
local_merge(const char *user, sets_t *s, const char *lang) {
  char path[PATH_MAX], local[PATH_MAX];
  struct dirent *e;
  DIR *dir;

  snprintf(path, sizeof(path), HOME "/%s/trophy2/nobackup/data", user);
  plat_path(path, local, sizeof(local));
  if(!(dir = opendir(local))) {
    return;
  }
  while((e = readdir(dir))) {
    int total[4] = {0}, got[4] = {0}, n, sum = 0, had = 0;
    jval_t *conf = NULL, *meta = NULL;
    unlock_t *u = NULL;
    int64_t newest = 0;
    set_t *t = NULL;
    FILE *f;

    if(!is_set_id(e->d_name) || !(f = ucp_open(e->d_name))) {
      continue;
    }
    if((n = dat_read(user, e->d_name, &u)) < 0 || !(conf = ucp_json(f, "tropconf.json"))) {
      goto next;
    }
    for(const jval_t *tr = first_trophy(conf); tr; tr = tr->next) {
      int g = grade_index(json_get_str(tr, "grade", ""));
      const unlock_t *k = unlock_of(u, n, atoi(json_get_str(tr, "id", "-1")));

      if(g < 0) {
        continue;
      }
      total[g]++;
      if(k && k->got) {
        got[g]++;
        sum++;
        if(k->when > newest) {
          newest = k->when;
        }
      }
    }
    for(size_t i = 0; i < s->n && !t; i++) {
      const char *colon = strchr(s->v[i].id, ':');
      if(!strcmp(colon ? colon + 1 : s->v[i].id, e->d_name)) {
        t = &s->v[i];
      }
    }
    if(!t) {
      snprintf(path, sizeof(path), "20:%s", e->d_name);
      if(!(t = set_for(s, path, 1))) {
        goto next;
      }
    }
    for(int g = 0; g < 4; g++) {
      had += t->got[g];
    }
    t->local_icon = 1;
    if(!t->name[0] && (meta = ucp_meta(f, conf, lang))) {
      const jval_t *m = json_get(json_get(meta, "metadata"), "titleMetadata");
      snprintf(t->name, sizeof(t->name), "%s", json_get_str(m, "name", ""));
    }
    if(!t->has_record || sum >= had) {
      memcpy(t->total, total, sizeof(total));
      memcpy(t->got, got, sizeof(got));
      t->progress = progress_of(total, got);
      t->local = 1;
      if(newest) {
        iso_time(newest, t->updated, sizeof(t->updated));
      }
    }
  next:
    json_free(meta);
    json_free(conf);
    free(u);
    fclose(f);
  }
  closedir(dir);
}

/* Which installed game each set belongs to: a game says so itself, in
 * sce_sys/trophy2/npbind.dat, which is all the console has to go by for a
 * set PlayStation Network never told it about. */
static void
bind_games(sets_t *s) {
  char path[PATH_MAX], local[PATH_MAX], set[16];
  struct dirent *e;
  DIR *dir;

  plat_path("/user/app", local, sizeof(local));
  if(!(dir = opendir(local))) {
    return;
  }
  while((e = readdir(dir))) {
    char *data = NULL, *at = NULL;
    size_t len = 0;

    if(!is_title_id(e->d_name)) {
      continue;
    }
    snprintf(path, sizeof(path), "/user/app/%s/sce_sys/trophy2/npbind.dat", e->d_name);
    plat_path(path, local, sizeof(local));
    if(fs_read_file(local, &data, &len, 64 * 1024)) {
      continue;
    }
    for(size_t i = 0; i + 12 <= len && !at; i++) {
      if(!memcmp(data + i, "NPWR", 4)) {
        snprintf(set, sizeof(set), "%.12s", data + i);
        at = is_set_id(set) ? set : NULL;
      }
    }
    for(size_t i = 0; at && i < s->n; i++) {
      set_t *t = &s->v[i];
      const char *colon = strchr(t->id, ':');

      if(!strcmp(colon ? colon + 1 : t->id, set) && !strstr(t->games, e->d_name) &&
         strlen(t->games) + 11 < sizeof(t->games)) {
        if(t->games[0]) {
          strcat(t->games, " ");
        }
        strcat(t->games, e->d_name);
      }
    }
    free(data);
  }
  closedir(dir);
}

static void
lang_of(http_req_t *req, char *lang, size_t size) {
  if(!http_query(req, "lang", lang, size)) {
    lang[0] = 0;
  }
  for(char *p = lang; *p; p++) {
    if(!isalnum((unsigned char)*p) && *p != '-') {
      lang[0] = 0;
      break;
    }
  }
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
  char local[PATH_MAX], path[PATH_MAX], lang[16];
  struct dirent *u;
  sets_t s = {0};
  DIR *home;
  jw_t w;

  if(!(s.v = malloc(MAX_SETS * sizeof(set_t)))) {
    http_send_error(req, 500, "out of memory");
    return;
  }
  lang_of(req, lang, sizeof(lang));
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
      s.n = 0;
      if((db = sq_open(local))) {
        sq_table(db, "titles", row_title, &s);
        sq_table(db, "title_records", row_record, &s);
        sq_table(db, "id_bindings", row_binding, &s);
        sq_close(db);
      }
      local_merge(u->d_name, &s, lang);
      bind_games(&s);
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
        jw_kbool(&w, "local", t->local);
        jw_kbool(&w, "localIcon", t->local_icon);
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

/* GET /api/trophies/set?user=1d31cd9c&id=NPWR12345_00[&lang=es-ES]
 * every trophy of a set: grade, name, and whether this user has it */
void
api_trophies_set(http_req_t *req) {
  static const char *const GRADES[] = {"platinum", "gold", "silver", "bronze"};
  char user[16], set[16], lang[16];
  jval_t *conf = NULL, *meta = NULL;
  const jval_t *names, *about;
  unlock_t *u = NULL;
  FILE *f;
  jw_t w;
  int n;

  lang_of(req, lang, sizeof(lang));
  if(!http_query(req, "user", user, sizeof(user)) || !is_user_id(user) ||
     !http_query(req, "id", set, sizeof(set)) || !is_set_id(set)) {
    http_send_error(req, 400, "invalid user or id");
    return;
  }
  if(!(f = ucp_open(set)) || !(conf = ucp_json(f, "tropconf.json"))) {
    if(f) {
      fclose(f);
    }
    http_send_error(req, 404, "the console does not have the trophies of %s", set);
    return;
  }
  meta = ucp_meta(f, conf, lang);
  fclose(f);
  n = dat_read(user, set, &u);
  about = json_get(meta, "metadata");
  names = json_get(about, "trophyMetadata");

  api_begin(&w);
  jw_kstr(&w, "id", set);
  jw_kstr(&w, "name", json_get_str(json_get(about, "titleMetadata"), "name", ""));
  jw_key(&w, "trophies");
  jw_arr(&w);
  for(const jval_t *tr = first_trophy(conf); tr; tr = tr->next) {
    const char *id = json_get_str(tr, "id", "");
    int g = grade_index(json_get_str(tr, "grade", ""));
    const unlock_t *k = unlock_of(u, n, atoi(id));
    char when[40] = "";

    if(g < 0) {
      continue;
    }
    jw_obj(&w);
    jw_kstr(&w, "id", id);
    jw_kstr(&w, "grade", GRADES[g]);
    jw_kbool(&w, "hidden", json_get_bool(tr, "hidden", 0));
    for(const jval_t *m = names ? names->child : NULL; m; m = m->next) {
      if(!strcmp(json_get_str(m, "id", ""), id)) {
        jw_kstr(&w, "name", json_get_str(m, "name", ""));
        jw_kstr(&w, "detail", json_get_str(m, "detail", ""));
        break;
      }
    }
    jw_kbool(&w, "got", k && k->got);
    if(k && k->got) {
      iso_time(k->when, when, sizeof(when));
    }
    jw_kstr(&w, "when", when);
    jw_obj_end(&w);
  }
  jw_arr_end(&w);
  json_free(meta);
  json_free(conf);
  free(u);
  api_finish(req, &w);
}

/* GET /api/trophies/icon?set=NPWR12345_00[&id=0007]
 * the picture of a trophy, or of the whole set without id */
void
api_trophies_icon(http_req_t *req) {
  char set[16], id[8] = "", name[48];
  char *png = NULL;
  size_t len = 0;
  FILE *f;

  http_query(req, "id", id, sizeof(id));
  if(!http_query(req, "set", set, sizeof(set)) || !is_set_id(set) ||
     strspn(id, "0123456789") != strlen(id)) {
    http_send_error(req, 400, "invalid set or id");
    return;
  }
  if((f = ucp_open(set))) {
    if(id[0]) {
      snprintf(name, sizeof(name), "trop%04d.png", atoi(id));
      png = ucp_read(f, name, 8 << 20, &len);
    } else {
      jval_t *conf = ucp_json(f, "tropconf.json");
      const char *try[] = {json_get_str(conf, "defaultLanguage", ""), "en-US"};

      for(size_t i = 0; i < 2 && !png; i++) {
        snprintf(name, sizeof(name), "icon0_%s.png", try[i]);
        png = ucp_read(f, name, 8 << 20, &len);
      }
      if(!png) {
        png = ucp_read(f, "icon0.png", 8 << 20, &len);
      }
      json_free(conf);
    }
    fclose(f);
  }
  if(!png) {
    http_send_error(req, 404, "no such icon");
    return;
  }
  http_send_head(req, 200, "image/png", (int64_t)len, "Cache-Control: max-age=604800\r\n");
  if(strcmp(req->method, "HEAD")) {
    http_send_all(req->fd, png, len);
  }
  free(png);
}
