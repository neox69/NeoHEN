/* NeoWEB - screenshots and video clips kept on the console.
 *
 * The console keeps them by game, with three small files next to each:
 *
 *   /user/av_contents/photo/<app>/<TITLE>/<xxx>/<date>_<time>_<n>.jxr|.jpg
 *   /user/av_contents/video/<app>/<TITLE>/<xxx>/<date>_<time>_<n>.webm|.mp4
 *                                               ...the same name with .dat
 *
 * The .dat says whose it is (the user, at byte 4) and the game's name (text
 * at byte 0x44, followed by the date). This lists them and sends them, one by one through
 * /fs/get or many as a ZIP grouped by user and game. Nothing is changed on
 * the console: its own gallery keeps an index of these files.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "log.h"
#include "plat.h"
#include "zipstream.h"

#define ROOT "/user/av_contents"
#define HOME "/user/home"

typedef struct capture {
  char path[300]; /* console path */
  char stem[48];  /* 20250615_150127_00997366 */
  char ext[8];
  char title_id[12];
  char title[96];
  char user[12];
  int video;
  uint64_t size;
  time_t mtime;
} capture_t;

typedef struct capture_list {
  capture_t *v;
  size_t n, cap;
} capture_list_t;

static const char *
media_ext(const char *name, int *video) {
  static const char *photo[] = {"jxr", "jpg", "jpeg", "png"};
  static const char *clip[] = {"webm", "mp4"};
  const char *dot = strrchr(name, '.');

  if(!dot) {
    return NULL;
  }
  for(size_t i = 0; i < sizeof(photo) / sizeof(photo[0]); i++) {
    if(!strcasecmp(dot + 1, photo[i])) {
      *video = 0;
      return photo[i];
    }
  }
  for(size_t i = 0; i < sizeof(clip) / sizeof(clip[0]); i++) {
    if(!strcasecmp(dot + 1, clip[i])) {
      *video = 1;
      return clip[i];
    }
  }
  return NULL;
}

/* The user and the game's name, from the .dat that goes with a capture. */
static void
read_dat(const char *local_media, capture_t *c) {
  char path[PATH_MAX];
  unsigned char raw[0x44 + 96];
  size_t n;
  FILE *f;

  snprintf(path, sizeof(path), "%s", local_media);
  char *dot = strrchr(path, '.');
  if(!dot || (size_t)(dot - path) + 5 > sizeof(path)) {
    return;
  }
  strcpy(dot, ".dat");
  if(!(f = fopen(path, "rb"))) {
    return;
  }
  n = fread(raw, 1, sizeof(raw), f);
  fclose(f);
  if(n >= 8) {
    snprintf(c->user, sizeof(c->user), "%02x%02x%02x%02x", raw[7], raw[6], raw[5], raw[4]);
  }
  if(n > 0x44) {
    size_t len = 0;
    while(0x44 + len < n && raw[0x44 + len] && len + 1 < sizeof(c->title)) {
      len++;
    }
    memcpy(c->title, raw + 0x44, len);
    c->title[len] = 0;
    /* it is the name of the capture: "<game>_<14 digits of date and time>" */
    if(len > 15 && c->title[len - 15] == '_' &&
       strspn(c->title + len - 14, "0123456789") == 14) {
      c->title[len - 15] = 0;
    }
  }
}

static int
captures_add(capture_list_t *l, const capture_t *c) {
  if(l->n == l->cap) {
    size_t cap = l->cap ? l->cap * 2 : 256;
    capture_t *v = realloc(l->v, cap * sizeof(capture_t));
    if(!v) {
      return -1;
    }
    l->v = v;
    l->cap = cap;
  }
  l->v[l->n++] = *c;
  return 0;
}

/* Goes down <kind>/<app>/<TITLE>/<xxx>/ and takes the media files there. */
static void
scan(capture_list_t *l, const char *console, int depth, const char *title_id) {
  char local[PATH_MAX];
  struct dirent *e;
  DIR *dir;

  plat_path(console, local, sizeof(local));
  if(!(dir = opendir(local))) {
    return;
  }
  while((e = readdir(dir))) {
    char child[PATH_MAX], child_local[PATH_MAX];
    struct stat st;

    if(e->d_name[0] == '.') {
      continue;
    }
    snprintf(child, sizeof(child), "%s/%s", console, e->d_name);
    plat_path(child, child_local, sizeof(child_local));
    if(stat(child_local, &st)) {
      continue;
    }
    if(S_ISDIR(st.st_mode)) {
      if(depth < 3) {
        /* depth 0 is the app's folder, 1 the game's, 2 the one above the files */
        scan(l, child, depth + 1, depth == 1 ? e->d_name : title_id);
      }
      continue;
    }
    if(depth == 3 && S_ISREG(st.st_mode) && strlen(child) < sizeof(((capture_t *)0)->path)) {
      capture_t c;
      const char *ext;

      memset(&c, 0, sizeof(c));
      if(!(ext = media_ext(e->d_name, &c.video))) {
        continue;
      }
      snprintf(c.path, sizeof(c.path), "%s", child);
      snprintf(c.stem, sizeof(c.stem), "%.*s", (int)(strrchr(e->d_name, '.') - e->d_name),
               e->d_name);
      snprintf(c.ext, sizeof(c.ext), "%s", ext);
      snprintf(c.title_id, sizeof(c.title_id), "%s", title_id ? title_id : "");
      c.size = (uint64_t)st.st_size;
      c.mtime = st.st_mtime;
      read_dat(child_local, &c);
      captures_add(l, &c);
    }
  }
  closedir(dir);
}

static void
scan_all(capture_list_t *l) {
  scan(l, ROOT "/photo", 0, NULL);
  scan(l, ROOT "/video", 0, NULL);
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

/* A name that can be a folder on any system. */
static void
safe_name(const char *in, char *out, size_t size) {
  size_t n = 0;

  for(const unsigned char *p = (const unsigned char *)in; *p && n + 1 < size; p++) {
    out[n++] = (*p < 0x20 || strchr("\\/:*?\"<>|", *p)) ? '_' : (char)*p;
  }
  while(n && (out[n - 1] == ' ' || out[n - 1] == '.')) {
    n--;
  }
  out[n] = 0;
  if(!n) {
    snprintf(out, size, "_");
  }
}

/* GET /api/captures/list */
void
api_captures_list(http_req_t *req) {
  capture_list_t l = {0};
  char seen[32][12];
  int nseen = 0;
  jw_t w;

  scan_all(&l);
  api_begin(&w);
  jw_key(&w, "items");
  jw_arr(&w);
  for(size_t i = 0; i < l.n; i++) {
    const capture_t *c = &l.v[i];
    int known = 0;

    jw_obj(&w);
    jw_kstr(&w, "id", c->stem);
    jw_kstr(&w, "kind", c->video ? "video" : "photo");
    jw_kstr(&w, "ext", c->ext);
    jw_kstr(&w, "titleId", c->title_id);
    jw_kstr(&w, "title", c->title);
    jw_kstr(&w, "user", c->user);
    jw_kstr(&w, "path", c->path);
    jw_kuint(&w, "size", c->size);
    jw_kint(&w, "mtime", (int64_t)c->mtime);
    jw_obj_end(&w);
    for(int k = 0; k < nseen; k++) {
      known |= !strcmp(seen[k], c->user);
    }
    if(!known && c->user[0] && nseen < 32) {
      snprintf(seen[nseen++], sizeof(seen[0]), "%s", c->user);
    }
  }
  jw_arr_end(&w);
  jw_key(&w, "users");
  jw_obj(&w);
  for(int k = 0; k < nseen; k++) {
    char name[40];
    user_name(seen[k], name, sizeof(name));
    jw_kstr(&w, seen[k], name);
  }
  jw_obj_end(&w);
  free(l.v);
  api_finish(req, &w);
}

static int
in_list(const char *list, const char *word) {
  size_t n = strlen(word);

  for(const char *p = list; (p = strstr(p, word)); p += n) {
    if((p == list || p[-1] == ',') && (!p[n] || p[n] == ',')) {
      return 1;
    }
  }
  return 0;
}

/* GET /api/captures/zip[?ids=a,b,c][&title=PPSA01234][&user=1d31cd9c][&kind=photo|video]
 * Everything that matches every filter given, as
 * <User>/<Game (TITLE)>/<file>. */
void
api_captures_zip(http_req_t *req) {
  char ids[8192] = "", title[16] = "", user[16] = "", kind[16] = "", name[64];
  capture_list_t l = {0};
  zlist_t z = {0};
  time_t now = time(NULL);
  struct tm tm;
  int bad = 0;

  http_query(req, "ids", ids, sizeof(ids));
  http_query(req, "title", title, sizeof(title));
  http_query(req, "user", user, sizeof(user));
  http_query(req, "kind", kind, sizeof(kind));
  scan_all(&l);
  for(size_t i = 0; i < l.n && !bad; i++) {
    const capture_t *c = &l.v[i];
    char who[40], who_safe[48], game[128], game_safe[140], path[PATH_MAX], local[PATH_MAX];

    if((ids[0] && !in_list(ids, c->stem)) || (title[0] && strcmp(title, c->title_id)) ||
       (user[0] && strcmp(user, c->user)) || (kind[0] && strcmp(kind, c->video ? "video" : "photo"))) {
      continue;
    }
    user_name(c->user[0] ? c->user : "unknown", who, sizeof(who));
    safe_name(who, who_safe, sizeof(who_safe));
    if(c->title[0]) {
      snprintf(game, sizeof(game), "%s (%s)", c->title, c->title_id);
    } else {
      snprintf(game, sizeof(game), "%s", c->title_id);
    }
    safe_name(game, game_safe, sizeof(game_safe));
    snprintf(path, sizeof(path), "%s/%s/%s.%s", who_safe, game_safe, c->stem, c->ext);
    plat_path(c->path, local, sizeof(local));
    bad = zlist_add(&z, path, local, NULL, c->size, c->mtime);
  }
  free(l.v);
  if(bad || !z.n) {
    zlist_free(&z);
    http_send_error(req, bad ? 500 : 404, bad ? "out of memory" : "no captures match");
    return;
  }
  localtime_r(&now, &tm);
  strftime(name, sizeof(name), "NeoHEN-captures-%Y%m%d-%H%M%S.zip", &tm);
  zip_send(req, &z, name);
  zlist_free(&z);
}
