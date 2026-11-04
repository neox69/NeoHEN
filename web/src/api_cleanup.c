/* NeoWEB - duplicates and leftovers in the library.
 *
 * GET /api/library/cleanup lists, and never deletes:
 *
 * duplicates: one title id found in more than one place. A copy is
 *   - the game installed by the console: /user/app/<ID> with no ShadowMount+
 *     link in it, or <ext|usb>/user/app/<ID> on extended storage;
 *   - the image or folder ShadowMount+ registered it from (mount_img.lnk,
 *     mount.lnk in /user/app/<ID>): the "active" one;
 *   - an image file or game folder (one with an eboot.bin) at the top of a
 *     drive ShadowMount+ reads images from (the drives of api_games.c). A
 *     folder's id is the titleId in its sce_sys/param.json; an image's id is
 *     the one its registration points at, or failing that a title id written
 *     in its file name on its own.
 *   Only those last ones, when they are not the active copy, can be deleted
 *   from the page; the rest are said, not touched.
 *
 * leftovers: things no game uses any more.
 *   - "<name>.subiendo": an upload from the page's "Add game" that never got
 *     renamed to its final name, last written more than an hour ago (the
 *     page resumes one when the same file is picked again);
 *   - "<name>.moving": a copy of /api/games/move that did not finish;
 *   - /user/patch/<ID>, /user/addcont/<ID> (and the same on extended
 *     storage) for a title id with no game anywhere: not in any app folder
 *     and with no /user/appmeta/<ID> either (the console keeps that for a
 *     game on a drive that is not plugged in);
 *   - <drive>/backports/<ID>[.off] for such a title id;
 *   - NeoWEB's own copy of an image's fakelib (/data/neohen/web/fakelib/<ID>)
 *     for a title no longer registered.
 *
 * Deleting goes through POST /api/fs/delete, with its rules, after the page
 * asks.
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
#include <unistd.h>

#include "fsutil.h"
#include "plat.h"

#define STALE_S   3600 /* an upload not written to for this long has stopped */
#define MAX_COPIES 2048

/* the drives ShadowMount+ images live on, as in api_games.c */
static const char *const IMG_ROOTS[] = {
  "/data/homebrew", "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/ext1",
};
#define N_IMG_ROOTS (sizeof(IMG_ROOTS) / sizeof(IMG_ROOTS[0]))

/* where the console installs games, as in api_apps.c */
static const char *const APP_ROOTS[] = {
  "/user/app", "/mnt/ext0/user/app", "/mnt/ext1/user/app", "/mnt/usb0/user/app",
  "/mnt/usb1/user/app",
};
#define N_APP_ROOTS (sizeof(APP_ROOTS) / sizeof(APP_ROOTS[0]))

/* patches and add-ons, by title id, on the internal drive and extended storage */
static const char *const EXTRA_ROOTS[][2] = {
  {"/user/patch", "patch"},             {"/user/addcont", "addcont"},
  {"/mnt/ext0/user/patch", "patch"},    {"/mnt/ext0/user/addcont", "addcont"},
  {"/mnt/ext1/user/patch", "patch"},    {"/mnt/ext1/user/addcont", "addcont"},
};
#define N_EXTRA_ROOTS (sizeof(EXTRA_ROOTS) / sizeof(EXTRA_ROOTS[0]))

static const char *const IMG_EXT[] = {".ffpfsc", ".ffpfs", ".ffpkg", ".exfat", ".ufs", ".pfs"};

typedef struct copy {
  char id[12];
  char path[PATH_MAX];
  const char *kind;   /* installed, extended, image, folder */
  int active, deletable;
  dev_t dev;          /* the same file can be named two ways (/data is */
  ino_t ino;          /* /user/data): it is told by these */
} copy_t;

typedef struct scan {
  copy_t *c;
  size_t n;
} scan_t;

static int
lstat_console(const char *console, struct stat *st) {
  char local[PATH_MAX];
  plat_path(console, local, sizeof(local));
  return lstat(local, st);
}

static int
is_dir(const char *console) {
  struct stat st;
  return !lstat_console(console, &st) && S_ISDIR(st.st_mode);
}

static int
exists(const char *console) {
  struct stat st;
  return !lstat_console(console, &st);
}

static int
ends_with(const char *s, const char *tail) {
  size_t a = strlen(s), b = strlen(tail);
  return a > b && !strcasecmp(s + a - b, tail);
}

static int
first_line(const char *console, char *out, size_t size) {
  char local[PATH_MAX];
  char *data = NULL;
  size_t len = 0;

  out[0] = 0;
  plat_path(console, local, sizeof(local));
  if(fs_read_file(local, &data, &len, 8192)) {
    return -1;
  }
  data[strcspn(data, "\r\n")] = 0;
  snprintf(out, size, "%s", data);
  free(data);
  return out[0] == '/' && !strstr(out, "/../") ? 0 : -1;
}

/* an empty mount point is not a drive (as api_games.c tells) */
static int
root_usable(const char *console) {
  char local[PATH_MAX], up[PATH_MAX], uplocal[PATH_MAX];
  struct stat a, b;
  const char *slash;

  plat_path(console, local, sizeof(local));
  if(stat(local, &a) || !S_ISDIR(a.st_mode)) {
    return 0;
  }
  if(strncmp(console, "/mnt/", 5) || strcmp(plat_name(), "ps5")) {
    return 1;
  }
  slash = strrchr(console, '/');
  snprintf(up, sizeof(up), "%.*s", (int)(slash - console), console);
  plat_path(up, uplocal, sizeof(uplocal));
  return !stat(uplocal, &b) && a.st_dev != b.st_dev;
}

/* a title id written somewhere in a file name, on its own (not inside a
 * longer run of letters and digits) */
static int
id_in_name(const char *name, char *out) {
  size_t n = strlen(name);
  for(size_t i = 0; i + 9 <= n; i++) {
    char cand[10];
    if(i && isalnum((unsigned char)name[i - 1])) {
      continue;
    }
    if(i + 9 < n && isalnum((unsigned char)name[i + 9])) {
      continue;
    }
    memcpy(cand, name + i, 9);
    cand[9] = 0;
    if(is_title_id(cand)) {
      memcpy(out, cand, 10);
      return 0;
    }
  }
  return -1;
}

/* titleId of the game folder at console/sce_sys/param.json */
static int
folder_id(const char *console, char *out) {
  char path[PATH_MAX], local[PATH_MAX];
  char *data = NULL;
  size_t len = 0;
  jval_t *pj;
  int r = -1;

  snprintf(path, sizeof(path), "%s/sce_sys/param.json", console);
  plat_path(path, local, sizeof(local));
  if(fs_read_file(local, &data, &len, 1 << 20)) {
    return -1;
  }
  if((pj = json_parse(data, len))) {
    const char *id = json_get_str(pj, "titleId", "");
    if(is_title_id(id)) {
      snprintf(out, 10, "%s", id);
      r = 0;
    }
    json_free(pj);
  }
  free(data);
  return r;
}

/* a game kept as a folder has its eboot.bin; websrv's homebrew (eboot.elf,
 * homebrew.js) is not a copy of any game, whatever its param.json says */
static int
game_folder(const char *console) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/eboot.elf", console);
  if(exists(path)) {
    return 0;
  }
  snprintf(path, sizeof(path), "%s/homebrew.js", console);
  if(exists(path)) {
    return 0;
  }
  snprintf(path, sizeof(path), "%s/eboot.bin", console);
  return exists(path);
}

/* a title is there when some app folder or its appmeta is */
static int
title_present(const char *id) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "/user/appmeta/%s", id);
  if(exists(path)) {
    return 1;
  }
  for(size_t i = 0; i < N_APP_ROOTS; i++) {
    snprintf(path, sizeof(path), "%s/%s", APP_ROOTS[i], id);
    if(exists(path)) {
      return 1;
    }
  }
  return 0;
}

static copy_t *
same_copy(scan_t *s, const char *path, const struct stat *st) {
  for(size_t i = 0; i < s->n; i++) {
    if(!strcmp(s->c[i].path, path) || (s->c[i].dev == st->st_dev && s->c[i].ino == st->st_ino)) {
      return &s->c[i];
    }
  }
  return NULL;
}

/* the copy already seen at path (or at another name for the same file) */
static copy_t *
find_copy(scan_t *s, const char *path) {
  char local[PATH_MAX];
  struct stat st;

  plat_path(path, local, sizeof(local));
  return stat(local, &st) ? NULL : same_copy(s, path, &st);
}

/* the copy at path, added when it is not there yet; NULL when there is
 * nothing at path */
static copy_t *
add_copy(scan_t *s, const char *id, const char *path, const char *kind) {
  char local[PATH_MAX];
  struct stat st;
  copy_t *c;

  plat_path(path, local, sizeof(local));
  if(stat(local, &st)) {
    return NULL;
  }
  if((c = same_copy(s, path, &st))) {
    return c;
  }
  if(s->n >= MAX_COPIES) {
    return NULL;
  }
  c = &s->c[s->n++];
  memset(c, 0, sizeof(*c));
  snprintf(c->id, sizeof(c->id), "%s", id);
  snprintf(c->path, sizeof(c->path), "%s", path);
  c->kind = kind;
  c->dev = st.st_dev;
  c->ino = st.st_ino;
  return c;
}

/* an installed game's folder holds more than its sce_sys */
static int
has_content(const char *console) {
  char local[PATH_MAX];
  struct dirent *e;
  int found = 0;
  DIR *d;

  plat_path(console, local, sizeof(local));
  if(!(d = opendir(local))) {
    return 0;
  }
  while(!found && (e = readdir(d))) {
    found = strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && strcmp(e->d_name, "sce_sys");
  }
  closedir(d);
  return found;
}

/* size, and the newest modification time anywhere inside */
static void
tree_info(const char *local, uint64_t *bytes, time_t *newest, int depth) {
  struct stat st;
  struct dirent *e;
  DIR *d;

  if(lstat(local, &st)) {
    return;
  }
  if(st.st_mtime > *newest) {
    *newest = st.st_mtime;
  }
  if(!S_ISDIR(st.st_mode)) {
    if(S_ISREG(st.st_mode)) {
      *bytes += (uint64_t)st.st_size;
    }
    return;
  }
  if(depth > 32 || !(d = opendir(local))) {
    return;
  }
  while((e = readdir(d))) {
    char child[PATH_MAX];
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    snprintf(child, sizeof(child), "%s/%s", local, e->d_name);
    tree_info(child, bytes, newest, depth + 1);
  }
  closedir(d);
}

static uint64_t
console_size(const char *console, time_t *newest) {
  char local[PATH_MAX];
  uint64_t bytes = 0;
  time_t t = 0;
  plat_path(console, local, sizeof(local));
  tree_info(local, &bytes, &t, 0);
  if(newest) {
    *newest = t;
  }
  return bytes;
}

/* ------------------------------------------------------------ duplicates */

static void
scan_copies(scan_t *s) {
  char path[PATH_MAX], src[PATH_MAX], local[PATH_MAX];
  struct dirent *e;
  DIR *d;

  /* what the console has installed, and what ShadowMount+ registered */
  for(size_t r = 0; r < N_APP_ROOTS; r++) {
    plat_path(APP_ROOTS[r], local, sizeof(local));
    if(!(d = opendir(local))) {
      continue;
    }
    while((e = readdir(d))) {
      copy_t *c;
      if(!is_title_id(e->d_name)) {
        continue;
      }
      snprintf(path, sizeof(path), "%s/%s", APP_ROOTS[r], e->d_name);
      if(!is_dir(path)) {
        continue;
      }
      if(r == 0) {
        char lnk[PATH_MAX + 32], mount[PATH_MAX];
        snprintf(lnk, sizeof(lnk), "%s/mount.lnk", path);
        first_line(lnk, mount, sizeof(mount));
        snprintf(lnk, sizeof(lnk), "%s/mount_img.lnk", path);
        if(!first_line(lnk, src, sizeof(src))) {
          if((c = add_copy(s, e->d_name, src, "image"))) {
            c->active = 1;
          }
          continue;
        }
        if(mount[0] && strncmp(mount, "/mnt/shadowmnt", 14)) {
          if((c = add_copy(s, e->d_name, mount, "folder"))) {
            c->active = 1;
          }
          continue;
        }
        if(mount[0]) {
          continue; /* registered, but not saying from where */
        }
      }
      if(!has_content(path)) {
        continue;
      }
      if((c = add_copy(s, e->d_name, path, r ? "extended" : "installed"))) {
        c->active = 1;
      }
    }
    closedir(d);
  }

  /* images and game folders at the top of the drives ShadowMount+ reads */
  for(size_t r = 0; r < N_IMG_ROOTS; r++) {
    if(!root_usable(IMG_ROOTS[r])) {
      continue;
    }
    plat_path(IMG_ROOTS[r], local, sizeof(local));
    if(!(d = opendir(local))) {
      continue;
    }
    while((e = readdir(d))) {
      struct stat st;
      char id[12];
      copy_t *c = NULL;
      int image = 0;

      if(e->d_name[0] == '.' || ends_with(e->d_name, ".subiendo") ||
         ends_with(e->d_name, ".moving")) {
        continue;
      }
      snprintf(path, sizeof(path), "%s/%s", IMG_ROOTS[r], e->d_name);
      if(lstat_console(path, &st)) {
        continue;
      }
      for(size_t i = 0; i < sizeof(IMG_EXT) / sizeof(IMG_EXT[0]); i++) {
        image |= ends_with(e->d_name, IMG_EXT[i]);
      }
      /* a registration that points at it says best what it is: add_copy
       * gives that one back, whatever id is asked for */
      if(S_ISREG(st.st_mode) && image) {
        if(!(c = find_copy(s, path)) && !id_in_name(e->d_name, id)) {
          c = add_copy(s, id, path, "image");
        }
      } else if(S_ISDIR(st.st_mode) && game_folder(path) && !folder_id(path, id)) {
        c = add_copy(s, id, path, "folder");
      }
      if(c && !c->active) {
        c->deletable = 1;
      }
    }
    closedir(d);
  }
}

static int
copy_cmp(const void *a, const void *b) {
  const copy_t *x = a, *y = b;
  int c = strcmp(x->id, y->id);
  if(c) {
    return c;
  }
  if(x->active != y->active) {
    return y->active - x->active;
  }
  return strcmp(x->path, y->path);
}

static const char *
where_of(const char *path) {
  if(!strncmp(path, "/mnt/usb", 8)) {
    return "usb";
  }
  if(!strncmp(path, "/mnt/ext0", 9)) {
    return "extended";
  }
  return !strncmp(path, "/mnt/ext", 8) ? "m2" : "internal";
}

static void
write_duplicates(jw_t *w, scan_t *s) {
  jw_key(w, "duplicates");
  jw_arr(w);
  for(size_t i = 0; i < s->n;) {
    size_t j = i;
    while(j < s->n && !strcmp(s->c[j].id, s->c[i].id)) {
      j++;
    }
    if(j - i > 1) {
      jw_obj(w);
      jw_kstr(w, "id", s->c[i].id);
      jw_key(w, "copies");
      jw_arr(w);
      for(size_t k = i; k < j; k++) {
        jw_obj(w);
        jw_kstr(w, "path", s->c[k].path);
        jw_kstr(w, "kind", s->c[k].kind);
        jw_kstr(w, "where", where_of(s->c[k].path));
        jw_kuint(w, "size", console_size(s->c[k].path, NULL));
        jw_kbool(w, "active", s->c[k].active);
        jw_kbool(w, "deletable", s->c[k].deletable);
        jw_obj_end(w);
      }
      jw_arr_end(w);
      jw_obj_end(w);
    }
    i = j;
  }
  jw_arr_end(w);
}

/* ------------------------------------------------------------ leftovers */

static void
leftover(jw_t *w, const char *path, const char *kind, const char *id, time_t now,
         int need_stale) {
  time_t newest = 0;
  uint64_t bytes = console_size(path, &newest);

  if(need_stale && newest && now - newest < STALE_S) {
    return; /* still being written, most likely */
  }
  jw_obj(w);
  jw_kstr(w, "path", path);
  jw_kstr(w, "kind", kind);
  if(id) {
    jw_kstr(w, "id", id);
  }
  jw_kstr(w, "where", where_of(path));
  jw_kuint(w, "size", bytes);
  jw_kint(w, "mtime", (int64_t)newest);
  if(!strcmp(kind, "backport")) {
    char stub[PATH_MAX + 64];
    snprintf(stub, sizeof(stub), "%s/fakelib/libScePlayGo.sprx", path);
    jw_kbool(w, "stub", exists(stub));
  }
  jw_obj_end(w);
}

static void
write_leftovers(jw_t *w) {
  char path[PATH_MAX], local[PATH_MAX];
  time_t now = time(NULL);
  struct dirent *e;
  DIR *d;

  jw_key(w, "leftovers");
  jw_arr(w);
  for(size_t r = 0; r < N_IMG_ROOTS; r++) {
    char bp[PATH_MAX];
    if(!root_usable(IMG_ROOTS[r])) {
      continue;
    }
    plat_path(IMG_ROOTS[r], local, sizeof(local));
    if((d = opendir(local))) {
      while((e = readdir(d))) {
        int up = ends_with(e->d_name, ".subiendo"), mv = ends_with(e->d_name, ".moving");
        if(e->d_name[0] == '.' || (!up && !mv)) {
          continue;
        }
        snprintf(path, sizeof(path), "%s/%s", IMG_ROOTS[r], e->d_name);
        leftover(w, path, up ? "upload" : "move", NULL, now, 1);
      }
      closedir(d);
    }
    snprintf(bp, sizeof(bp), "%s/backports", IMG_ROOTS[r]);
    plat_path(bp, local, sizeof(local));
    if((d = opendir(local))) {
      while((e = readdir(d))) {
        char id[16];
        snprintf(id, sizeof(id), "%.9s", e->d_name);
        if(!is_title_id(id) || (e->d_name[9] && strcmp(e->d_name + 9, ".off")) ||
           title_present(id)) {
          continue;
        }
        snprintf(path, sizeof(path), "%s/%s", bp, e->d_name);
        if(is_dir(path)) {
          leftover(w, path, "backport", id, now, 0);
        }
      }
      closedir(d);
    }
  }
  for(size_t r = 0; r < N_EXTRA_ROOTS; r++) {
    plat_path(EXTRA_ROOTS[r][0], local, sizeof(local));
    if(!(d = opendir(local))) {
      continue;
    }
    while((e = readdir(d))) {
      if(!is_title_id(e->d_name) || title_present(e->d_name)) {
        continue;
      }
      snprintf(path, sizeof(path), "%s/%s", EXTRA_ROOTS[r][0], e->d_name);
      if(is_dir(path)) {
        leftover(w, path, EXTRA_ROOTS[r][1], e->d_name, now, 0);
      }
    }
    closedir(d);
  }
  plat_path(PS5AIO_DATA_DIR "/fakelib", local, sizeof(local));
  if((d = opendir(local))) {
    while((e = readdir(d))) {
      char app[PATH_MAX];
      if(!is_title_id(e->d_name)) {
        continue;
      }
      snprintf(app, sizeof(app), "/user/app/%s", e->d_name);
      if(exists(app)) {
        continue;
      }
      snprintf(path, sizeof(path), PS5AIO_DATA_DIR "/fakelib/%s", e->d_name);
      leftover(w, path, "fakelib", e->d_name, now, 0);
    }
    closedir(d);
  }
  jw_arr_end(w);
}

/* GET /api/library/cleanup */
void
api_library_cleanup(http_req_t *req) {
  scan_t s = {0};
  jw_t w;

  if(!(s.c = calloc(MAX_COPIES, sizeof(*s.c)))) {
    http_send_error(req, 500, "out of memory");
    return;
  }
  scan_copies(&s);
  if(s.n) {
    qsort(s.c, s.n, sizeof(*s.c), copy_cmp);
  }
  api_begin(&w);
  jw_kint(&w, "staleAfter", STALE_S);
  write_duplicates(&w, &s);
  write_leftovers(&w);
  free(s.c);
  api_finish(req, &w);
}
