/* NeoWEB - the games ShadowMount+ serves: where each one is, the fix some of
 * them need to get past "wait for the content to install", and moving an
 * image from one drive to another.
 *
 * ShadowMount+ leaves, for every game it registers:
 *
 *   /user/app/<ID>/mount_img.lnk   first line: the image file it comes from
 *   /user/app/<ID>/mount.lnk       where it is (or will be) mounted; for a
 *                                  game kept as a folder, the folder itself
 *   /user/app/<ID>/sce_sys/        a copy of the game's sce_sys
 *
 * and lays <folder of the image>/backports/<ID>/ over the game when it is
 * launched (<ID>.off is the same folder, switched off by the Toolbox).
 *
 * The fix: a game whose PlayGo data says "more than what is here" waits for
 * an installation that will never come. A stub libScePlayGo.sprx in the
 * backport's fakelib answers "all of it is installed"; it needs to be told
 * how many chunks and scenarios the game has, which playgo-chunk.dat says.
 * The backport's fakelib takes the place of the one inside the image, it
 * is not added to it, so whatever the image brings has to be copied along.
 * An image is only mounted while its game is open: NeoWEB notes what its
 * fakelib holds the first time it sees the game running.
 *
 *   /data/neohen/fixes/libScePlayGo.sprx   the stub (not shipped with NeoWEB)
 *   /data/neohen/web/fakelib/<ID>/         what the image of <ID> brings
 *   /data/neohen/web/games.json            {"bad": {"PPSA01234": "internal"}}
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "fsutil.h"
#include "jobs.h"
#include "log.h"
#include "plat.h"

#define APPS     "/user/app"
#define FIXES    "/data/neohen/fixes"
#define SEEN     PS5AIO_DATA_DIR "/fakelib"
#define NOTES    PS5AIO_DATA_DIR "/games.json"
#define STUB     "libScePlayGo.sprx"
#define STUB_DAT "playgo_stub.dat"
#define MARGIN   (64ull << 20) /* room left on the drive after a move */

typedef struct game {
  char id[12];
  char source[PATH_MAX];   /* the image file, or the folder of the game */
  char mount[PATH_MAX];
  char backport[PATH_MAX]; /* as it is now: may end in .off */
  int is_image, has_backport, backport_off;
  int has_playgo;
  unsigned chunks, scenarios;
  uint64_t size;
} game_t;

/* the drives an image can live on: path, and what the page calls it */
static const char *const ROOTS[][2] = {
  {"/data/homebrew", "internal"}, {"/mnt/usb0", "usb"}, {"/mnt/usb1", "usb"},
  {"/mnt/usb2", "usb"},           {"/mnt/usb3", "usb"}, {"/mnt/ext1", "m2"},
};
#define NROOTS (sizeof(ROOTS) / sizeof(ROOTS[0]))

static int
exists(const char *console, struct stat *st) {
  char local[PATH_MAX];
  struct stat tmp;

  plat_path(console, local, sizeof(local));
  return !stat(local, st ? st : &tmp);
}

static int
is_dir(const char *console) {
  struct stat st;
  return exists(console, &st) && S_ISDIR(st.st_mode);
}

/* first line of a small text file */
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

static void
parent_of(const char *path, char *out, size_t size) {
  const char *slash = strrchr(path, '/');
  snprintf(out, size, "%.*s", slash && slash > path ? (int)(slash - path) : 1, path);
}

static const char *
where_of(const char *source) {
  if(!strncmp(source, "/mnt/usb", 8)) {
    return "usb";
  }
  return !strncmp(source, "/mnt/ext", 8) ? "m2" : "internal";
}

static int
game_load(const char *id, game_t *g) {
  char path[PATH_MAX], root[PATH_MAX], local[PATH_MAX];
  unsigned char head[16];
  struct stat st;
  FILE *f;

  memset(g, 0, sizeof(*g));
  if(!is_title_id(id)) {
    return -1;
  }
  snprintf(g->id, sizeof(g->id), "%s", id);
  snprintf(path, sizeof(path), APPS "/%s/mount.lnk", id);
  first_line(path, g->mount, sizeof(g->mount));
  snprintf(path, sizeof(path), APPS "/%s/mount_img.lnk", id);
  if(!first_line(path, g->source, sizeof(g->source))) {
    g->is_image = 1;
  } else if(g->mount[0] && strncmp(g->mount, "/mnt/shadowmnt", 14)) {
    snprintf(g->source, sizeof(g->source), "%s", g->mount);
  } else {
    return -1;
  }
  if(exists(g->source, &st) && S_ISREG(st.st_mode)) {
    g->size = (uint64_t)st.st_size;
  }

  snprintf(path, sizeof(path), APPS "/%s/sce_sys/playgo-chunk.dat", id);
  plat_path(path, local, sizeof(local));
  if((f = fopen(local, "rb"))) {
    if(fread(head, 1, sizeof(head), f) == sizeof(head) && !memcmp(head, "plgx", 4)) {
      g->has_playgo = 1;
      g->chunks = head[0x0a] | head[0x0b] << 8;
      g->scenarios = head[0x0e] | head[0x0f] << 8;
    }
    fclose(f);
  }

  parent_of(g->source, root, sizeof(root));
  snprintf(g->backport, sizeof(g->backport), "%s/backports/%s", root, id);
  if(is_dir(g->backport)) {
    g->has_backport = 1;
  } else {
    snprintf(path, sizeof(path), "%s.off", g->backport);
    if(is_dir(path)) {
      snprintf(g->backport, sizeof(g->backport), "%s", path);
      g->has_backport = g->backport_off = 1;
    }
  }
  return 0;
}

static int
is_running(const char *id) {
  char tid[16] = "";
  return !plat_app_running(tid, sizeof(tid), NULL, NULL) && !strcmp(tid, id);
}

/* ------------------------------------------------- what the image brings */

static int
seen(const char *id) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), SEEN "/%s/.seen", id);
  return exists(path, NULL);
}

/* The game is open, so its image is mounted: keep a copy of its fakelib. */
void
games_on_game_open(const char *title) {
  char path[PATH_MAX], from[PATH_MAX], to[PATH_MAX], src[PATH_MAX], dst[PATH_MAX];
  struct dirent *e;
  game_t g;
  DIR *dir;

  if(game_load(title, &g) || !g.mount[0]) {
    return;
  }
  snprintf(path, sizeof(path), "%s/sce_sys", g.mount);
  if(!is_dir(path)) {
    return; /* not mounted after all */
  }
  snprintf(path, sizeof(path), SEEN "/%s", g.id);
  plat_path(path, to, sizeof(to));
  fs_mkdirs(to, 0777);
  snprintf(path, sizeof(path), "%s/fakelib", g.mount);
  plat_path(path, from, sizeof(from));
  if((dir = opendir(from))) {
    while((e = readdir(dir))) {
      struct stat a, b;

      if(e->d_name[0] == '.') {
        continue;
      }
      snprintf(src, sizeof(src), "%s/%s", from, e->d_name);
      snprintf(dst, sizeof(dst), "%s/%s", to, e->d_name);
      if(stat(src, &a) || !S_ISREG(a.st_mode) || a.st_size > (64 << 20) ||
         (!stat(dst, &b) && b.st_size == a.st_size)) {
        continue;
      }
      if(fs_copy_file(src, dst, NULL)) {
        LOG_WARN("games: could not keep %s of %s: %s", e->d_name, g.id, strerror(errno));
        closedir(dir);
        return;
      }
    }
    closedir(dir);
  }
  snprintf(dst, sizeof(dst), "%s/.seen", to);
  fs_write_file(dst, "", 0);
}

/* names of the regular files in a folder, as a JSON array */
static int
write_names(jw_t *w, const char *console) {
  char local[PATH_MAX];
  struct dirent *e;
  int n = 0;
  DIR *dir;

  jw_arr(w);
  plat_path(console, local, sizeof(local));
  if((dir = opendir(local))) {
    while((e = readdir(dir))) {
      if(e->d_name[0] != '.') {
        jw_str(w, e->d_name);
        n++;
      }
    }
    closedir(dir);
  }
  jw_arr_end(w);
  return n;
}

/* ------------------------------------------------------------ the stub */

/* where a libScePlayGo.sprx can be taken from: the fixes folder, or a
 * backport the user already made by hand */
static int
find_stub(char *out, size_t size) {
  char path[PATH_MAX], local[PATH_MAX];
  struct dirent *e;
  DIR *dir;

  snprintf(out, size, FIXES "/" STUB);
  if(exists(out, NULL)) {
    return 0;
  }
  for(size_t i = 0; i < NROOTS; i++) {
    snprintf(path, sizeof(path), "%s/backports", ROOTS[i][0]);
    plat_path(path, local, sizeof(local));
    if(!(dir = opendir(local))) {
      continue;
    }
    while((e = readdir(dir))) {
      if(e->d_name[0] == '.') {
        continue;
      }
      snprintf(out, size, "%s/%s/fakelib/" STUB, path, e->d_name);
      if(exists(out, NULL)) {
        closedir(dir);
        return 0;
      }
    }
    closedir(dir);
  }
  out[0] = 0;
  return -1;
}

/* ------------------------------------------------------------ notes */

static jval_t *
notes_load(void) {
  char local[PATH_MAX];
  char *data = NULL;
  jval_t *v = NULL;
  size_t len = 0;

  plat_path(NOTES, local, sizeof(local));
  if(!fs_read_file(local, &data, &len, 1 << 20)) {
    v = json_parse(data, len);
    free(data);
  }
  if(!v || v->type != J_OBJ) {
    json_free(v);
    v = json_parse("{}", 2);
  }
  return v;
}

/* ------------------------------------------------------------ listing */

static void
write_game(jw_t *w, const game_t *g, const jval_t *notes) {
  char path[PATH_MAX], line[64] = "";
  int stub, dat;

  jw_obj(w);
  jw_kstr(w, "id", g->id);
  jw_kstr(w, "source", g->source);
  jw_kstr(w, "kind", g->is_image ? "image" : "folder");
  jw_kstr(w, "where", where_of(g->source));
  jw_kuint(w, "size", g->size);
  jw_kbool(w, "running", is_running(g->id));
  jw_kstr(w, "bad", json_get_str(json_get(notes, "bad"), g->id, ""));

  jw_key(w, "playgo");
  jw_obj(w);
  jw_kbool(w, "present", g->has_playgo);
  jw_kint(w, "chunks", g->chunks);
  jw_kint(w, "scenarios", g->scenarios);
  jw_obj_end(w);

  snprintf(path, sizeof(path), "%s/fakelib/" STUB, g->backport);
  stub = g->has_backport && exists(path, NULL);
  snprintf(path, sizeof(path), "%s/" STUB_DAT, g->backport);
  dat = 0;
  if(g->has_backport) {
    char local[PATH_MAX];
    char *data = NULL;
    size_t len = 0;

    /* two numbers, one per line: chunks and scenarios */
    plat_path(path, local, sizeof(local));
    if(!fs_read_file(local, &data, &len, 64)) {
      unsigned c = 0, s = 0;
      if(sscanf(data, "%u %u", &c, &s) == 2) {
        snprintf(line, sizeof(line), "%u/%u", c, s);
        dat = 1;
      }
      free(data);
    }
  }
  jw_key(w, "backport");
  jw_obj(w);
  jw_kstr(w, "path", g->backport);
  jw_kbool(w, "exists", g->has_backport);
  jw_kbool(w, "enabled", g->has_backport && !g->backport_off);
  jw_kbool(w, "playgo", stub && dat);
  jw_kstr(w, "values", line);
  jw_key(w, "fakelib");
  snprintf(path, sizeof(path), "%s/fakelib", g->backport);
  write_names(w, path);
  jw_obj_end(w);

  jw_key(w, "image");
  jw_obj(w);
  jw_kbool(w, "known", seen(g->id));
  jw_key(w, "fakelib");
  snprintf(path, sizeof(path), SEEN "/%s", g->id);
  write_names(w, path);
  jw_obj_end(w);
  jw_obj_end(w);
}

static int
root_usable(const char *console) {
  char local[PATH_MAX], up[PATH_MAX], uplocal[PATH_MAX];
  struct stat a, b;

  plat_path(console, local, sizeof(local));
  if(stat(local, &a) || !S_ISDIR(a.st_mode)) {
    return 0;
  }
  if(strncmp(console, "/mnt/", 5) || strcmp(plat_name(), "ps5")) {
    return 1;
  }
  /* an empty mount point is not a drive */
  parent_of(console, up, sizeof(up));
  plat_path(up, uplocal, sizeof(uplocal));
  return !stat(uplocal, &b) && a.st_dev != b.st_dev;
}

/* GET /api/games/images[?id=PPSA01234] */
void
api_games_images(http_req_t *req) {
  char local[PATH_MAX], only[16] = "", stub[PATH_MAX];
  jval_t *notes = notes_load();
  struct dirent *e;
  game_t g;
  DIR *dir;
  jw_t w;

  http_query(req, "id", only, sizeof(only));
  api_begin(&w);
  jw_kbool(&w, "stub", !find_stub(stub, sizeof(stub)));
  jw_kstr(&w, "stubFolder", FIXES);
  jw_key(&w, "roots");
  jw_arr(&w);
  for(size_t i = 0; i < NROOTS; i++) {
    struct statvfs sv;

    if(!root_usable(ROOTS[i][0])) {
      continue;
    }
    plat_path(ROOTS[i][0], local, sizeof(local));
    jw_obj(&w);
    jw_kstr(&w, "path", ROOTS[i][0]);
    jw_kstr(&w, "where", ROOTS[i][1]);
    if(!statvfs(local, &sv)) {
      jw_kuint(&w, "free", (uint64_t)sv.f_bavail * sv.f_frsize);
      jw_kuint(&w, "total", (uint64_t)sv.f_blocks * sv.f_frsize);
    }
    jw_obj_end(&w);
  }
  jw_arr_end(&w);
  jw_key(&w, "games");
  jw_arr(&w);
  if(only[0]) {
    if(!game_load(only, &g)) {
      write_game(&w, &g, notes);
    }
  } else {
    plat_path(APPS, local, sizeof(local));
    if((dir = opendir(local))) {
      while((e = readdir(dir))) {
        if(!game_load(e->d_name, &g)) {
          write_game(&w, &g, notes);
        }
      }
      closedir(dir);
    }
  }
  jw_arr_end(&w);
  json_free(notes);
  api_finish(req, &w);
}

static void
reply_game(http_req_t *req, const char *id) {
  jval_t *notes = notes_load();
  game_t g;
  jw_t w;

  api_begin(&w);
  if(!game_load(id, &g)) {
    jw_key(&w, "game");
    write_game(&w, &g, notes);
  }
  json_free(notes);
  api_finish(req, &w);
}

/* the game named in the body, or an error already sent */
static int
body_game(http_req_t *req, jval_t **body, game_t *g) {
  if(!(*body = api_body(req))) {
    return -1;
  }
  if(game_load(json_get_str(*body, "id", ""), g)) {
    json_free(*body);
    http_send_error(req, 404, "not a game served by ShadowMount+");
    return -1;
  }
  return 0;
}

/* POST /api/games/peek {"id": "PPSA01234"}
 * the image is mounted right now (the page asked ShadowMount+ to): keep
 * what its fakelib holds, as when the game is seen open */
void
api_games_peek(http_req_t *req) {
  jval_t *b;
  game_t g;

  if(body_game(req, &b, &g)) {
    return;
  }
  json_free(b);
  games_on_game_open(g.id);
  if(!seen(g.id)) {
    http_send_error(req, 409, "the image of %s is not mounted", g.id);
    return;
  }
  reply_game(req, g.id);
}

/* ------------------------------------------------------------ the fix */

static int
copy_missing(const char *from_console, const char *to_console) {
  char from[PATH_MAX], to[PATH_MAX];
  struct stat st;

  plat_path(from_console, from, sizeof(from));
  plat_path(to_console, to, sizeof(to));
  if(!stat(to, &st)) {
    return 0;
  }
  return fs_copy_file(from, to, NULL);
}

/* POST /api/games/fix {"id": "PPSA01234"[, "force": true]}
 * "force" applies it without knowing what the image brings in its fakelib */
static void fix_game(http_req_t *req, game_t g, int force);

void
api_games_fix(http_req_t *req) {
  jval_t *b;
  game_t g;
  int force;

  if(body_game(req, &b, &g)) {
    return;
  }
  force = json_get_bool(b, "force", 0);
  json_free(b);
  fix_game(req, g, force);
}

/* POST /api/games/autofix {"id": "PPSA01234"}
 * the whole fix in one go, for the Toolbox: when the image has not been seen
 * yet it is mounted through ShadowMount+ for a moment to see what it brings */
void
api_games_autofix(http_req_t *req) {
  char body[64], err[160], path[PATH_MAX];
  jval_t *b;
  game_t g;

  if(body_game(req, &b, &g)) {
    return;
  }
  json_free(b);
  if(g.has_playgo && g.is_image && !seen(g.id) && !is_running(g.id)) {
    snprintf(body, sizeof(body), "{\"title_id\":\"%s\"}", g.id);
    if(smp_call("/api/v1/games/mount", body, NULL, err, sizeof(err))) {
      http_send_error(req, 502, "the image could not be mounted to look at it: %s", err);
      return;
    }
    games_on_game_open(g.id);
    if(smp_call("/api/v1/games/unmount", body, NULL, err, sizeof(err))) {
      LOG_WARN("games: %s stays mounted: %s", g.id, err);
    }
    if(!seen(g.id)) {
      http_send_error(req, 409, "the image of %s was mounted but could not be read", g.id);
      return;
    }
  }
  snprintf(path, sizeof(path), SEEN "/%s/" STUB, g.id);
  if(exists(path, NULL)) {
    http_send_error(req, 409, "the image of %s already brings its own fix", g.id);
    return;
  }
  fix_game(req, g, 0);
}

static void
fix_game(http_req_t *req, game_t g, int force) {
  char path[PATH_MAX], other[PATH_MAX], local[PATH_MAX], stub[PATH_MAX], text[64];
  struct dirent *e;
  DIR *dir;

  if(!g.has_playgo) {
    http_send_error(req, 409, "%s has no PlayGo data: this fix is not for it", g.id);
    return;
  }
  if(find_stub(stub, sizeof(stub))) {
    http_send_error(req, 409, "%s is missing: put it in %s", STUB, FIXES);
    return;
  }
  if(is_running(g.id)) {
    games_on_game_open(g.id);
  }
  snprintf(path, sizeof(path), SEEN "/%s/" STUB, g.id);
  if(exists(path, NULL)) {
    http_send_error(req, 409, "the image of %s already brings its own %s", g.id, STUB);
    return;
  }
  if(!seen(g.id) && !force) {
    http_send_error(req, 409, "open %s once first, so that what its image brings can be kept",
                    g.id);
    return;
  }

  if(g.backport_off) {
    char on[PATH_MAX], a[PATH_MAX], c[PATH_MAX];

    snprintf(on, sizeof(on), "%.*s", (int)(strlen(g.backport) - 4), g.backport);
    plat_path(g.backport, a, sizeof(a));
    plat_path(on, c, sizeof(c));
    if(rename(a, c)) {
      http_send_error(req, 500, "%s: %s", g.backport, strerror(errno));
      return;
    }
    snprintf(g.backport, sizeof(g.backport), "%s", on);
  }
  snprintf(path, sizeof(path), "%s/fakelib", g.backport);
  plat_path(path, local, sizeof(local));
  if(fs_mkdirs(local, 0777)) {
    http_send_error(req, 500, "%s: %s", path, strerror(errno));
    return;
  }
  /* keep the stub where the next game will find it */
  plat_path(FIXES, local, sizeof(local));
  fs_mkdirs(local, 0777);
  copy_missing(stub, FIXES "/" STUB);

  snprintf(path, sizeof(path), "%s/fakelib/" STUB, g.backport);
  if(copy_missing(stub, path)) {
    http_send_error(req, 500, "%s: %s", path, strerror(errno));
    return;
  }
  snprintf(path, sizeof(path), "%s/" STUB_DAT, g.backport);
  plat_path(path, local, sizeof(local));
  snprintf(text, sizeof(text), "%u\n%u\n", g.chunks, g.scenarios);
  if(fs_write_file(local, text, strlen(text))) {
    http_send_error(req, 500, "%s: %s", path, strerror(errno));
    return;
  }
  /* the image's own libraries, which this fakelib now stands in for */
  snprintf(path, sizeof(path), SEEN "/%s", g.id);
  plat_path(path, local, sizeof(local));
  if((dir = opendir(local))) {
    while((e = readdir(dir))) {
      if(e->d_name[0] == '.') {
        continue;
      }
      snprintf(path, sizeof(path), SEEN "/%s/%s", g.id, e->d_name);
      snprintf(other, sizeof(other), "%s/fakelib/%s", g.backport, e->d_name);
      if(copy_missing(path, other)) {
        closedir(dir);
        http_send_error(req, 500, "%s: %s", other, strerror(errno));
        return;
      }
    }
    closedir(dir);
  }
  LOG_INFO("games: PlayGo fix for %s (%u chunks, %u scenarios)", g.id, g.chunks, g.scenarios);
  reply_game(req, g.id);
}

/* POST /api/games/unfix {"id": "PPSA01234"}
 * takes away what the fix put, and nothing else that may be in the backport */
void
api_games_unfix(http_req_t *req) {
  char path[PATH_MAX], other[PATH_MAX], local[PATH_MAX], kept[PATH_MAX];
  struct dirent *e;
  jval_t *b;
  game_t g;
  DIR *dir;

  if(body_game(req, &b, &g)) {
    return;
  }
  json_free(b);
  if(!g.has_backport) {
    reply_game(req, g.id);
    return;
  }
  snprintf(path, sizeof(path), "%s/fakelib/" STUB, g.backport);
  plat_path(path, local, sizeof(local));
  unlink(local);
  snprintf(path, sizeof(path), "%s/" STUB_DAT, g.backport);
  plat_path(path, local, sizeof(local));
  unlink(local);
  /* the copies of what the image brings: only those still equal in size */
  snprintf(path, sizeof(path), SEEN "/%s", g.id);
  plat_path(path, kept, sizeof(kept));
  if((dir = opendir(kept))) {
    while((e = readdir(dir))) {
      struct stat a, c;

      if(e->d_name[0] == '.') {
        continue;
      }
      snprintf(other, sizeof(other), "%s/%s", kept, e->d_name);
      snprintf(path, sizeof(path), "%s/fakelib/%s", g.backport, e->d_name);
      plat_path(path, local, sizeof(local));
      if(!stat(other, &a) && !stat(local, &c) && a.st_size == c.st_size) {
        unlink(local);
      }
    }
    closedir(dir);
  }
  /* empty folders go; anything the user put there stays */
  snprintf(path, sizeof(path), "%s/fakelib", g.backport);
  plat_path(path, local, sizeof(local));
  rmdir(local);
  plat_path(g.backport, local, sizeof(local));
  rmdir(local);
  LOG_INFO("games: PlayGo fix removed from %s", g.id);
  reply_game(req, g.id);
}

/* POST /api/games/note {"id": "PPSA01234", "bad": "internal" | "usb" | ""}
 * the user's own note: from that kind of drive this game does not work */
void
api_games_note(http_req_t *req) {
  char local[PATH_MAX];
  jval_t *b, *notes;
  const char *bad;
  game_t g;
  jw_t w;

  if(body_game(req, &b, &g)) {
    return;
  }
  bad = json_get_str(b, "bad", "");
  if(strcmp(bad, "") && strcmp(bad, "internal") && strcmp(bad, "usb") && strcmp(bad, "m2")) {
    json_free(b);
    http_send_error(req, 400, "bad must be internal, usb, m2 or empty");
    return;
  }
  notes = notes_load();
  json_set_str(json_get_or_add_obj(notes, "bad"), g.id, bad);
  jw_init(&w);
  json_write(&w, notes);
  plat_path(NOTES, local, sizeof(local));
  if(fs_write_file(local, w.data, w.len)) {
    http_send_error(req, 500, "%s: %s", NOTES, strerror(errno));
  } else {
    reply_game(req, g.id);
  }
  jw_free(&w);
  json_free(notes);
  json_free(b);
}

/* ------------------------------------------------------------ moving */

typedef struct move_arg {
  char id[12];
  char src[PATH_MAX], dst[PATH_MAX], tmp[PATH_MAX];   /* local paths */
  char bp_src[PATH_MAX], bp_dst[PATH_MAX];            /* local; empty = none */
  char result[PATH_MAX];                              /* console path */
} move_arg_t;

static int
move_job(job_t *job, void *p) {
  move_arg_t *a = p;
  struct stat in, out;
  uint64_t extra = 0;
  uint32_t files = 0;

  if(stat(a->src, &in)) {
    return job_fail(job, "the image is gone: %s", strerror(errno));
  }
  if(a->bp_src[0]) {
    fs_tree_size(a->bp_src, &extra, &files, job);
  }
  job->bytes_total = (uint64_t)in.st_size + extra;
  job->files_total = 1 + files;

  /* under another name until it is whole, so that ShadowMount+ leaves it be */
  job_set_current(job, "%s", a->result);
  if(fs_copy_file(a->src, a->tmp, job) || stat(a->tmp, &out) || out.st_size != in.st_size) {
    int err = errno;
    unlink(a->tmp);
    return job->cancel ? -1 : job_fail(job, "copy: %s", strerror(err));
  }
  job->files_done = 1;
  if(a->bp_src[0] && (fs_mkparents(a->bp_dst) || fs_copy_tree(a->bp_src, a->bp_dst, 0, job))) {
    int err = errno;
    unlink(a->tmp);
    fs_remove_tree(a->bp_dst, NULL);
    return job->cancel ? -1 : job_fail(job, "backport: %s", strerror(err));
  }
  if(rename(a->tmp, a->dst)) {
    int err = errno;
    unlink(a->tmp);
    if(a->bp_src[0]) {
      fs_remove_tree(a->bp_dst, NULL);
    }
    return job_fail(job, "rename: %s", strerror(err));
  }
  /* the copy is in place and complete: only now does the original go */
  if(unlink(a->src)) {
    return job_fail(job, "moved, but the original could not be removed: %s", strerror(errno));
  }
  if(a->bp_src[0]) {
    fs_remove_tree(a->bp_src, NULL);
  }
  job->bytes_done = job->bytes_total;
  job->current[0] = 0;
  job_set_result(job, "%s", a->result);
  LOG_INFO("games: %s moved to %s", a->id, a->result);
  return 0;
}

/* POST /api/games/move {"id": "PPSA01234", "to": "/mnt/usb0"} */
void
api_games_move(http_req_t *req) {
  char path[PATH_MAX], root[PATH_MAX], local[PATH_MAX], title[600];
  const char *to, *name;
  struct statvfs sv;
  move_arg_t *a;
  jval_t *b;
  game_t g;
  int id, ok = 0;
  jw_t w;

  if(body_game(req, &b, &g)) {
    return;
  }
  to = json_get_str(b, "to", "");
  for(size_t i = 0; i < NROOTS; i++) {
    ok |= !strcmp(to, ROOTS[i][0]) && root_usable(to);
  }
  parent_of(g.source, root, sizeof(root));
  name = strrchr(g.source, '/') + 1;
  snprintf(path, sizeof(path), "%s/%s", to, name);
  plat_path(to, local, sizeof(local));

  if(!ok) {
    http_send_error(req, 400, "'to' is not a drive images can live on");
  } else if(!g.is_image || !g.size) {
    http_send_error(req, 409, "only games kept as an image file can be moved");
  } else if(!strcmp(root, to)) {
    http_send_error(req, 409, "%s is already there", g.id);
  } else if(is_running(g.id)) {
    http_send_error(req, 409, "close %s first", g.id);
  } else if(exists(path, NULL)) {
    http_send_error(req, 409, "%s already exists", path);
  } else if(statvfs(local, &sv) || (uint64_t)sv.f_bavail * sv.f_frsize < g.size + MARGIN) {
    http_send_error(req, 409, "not enough room in %s", to);
  } else if(!(a = calloc(1, sizeof(*a)))) {
    http_send_error(req, 500, "out of memory");
  } else {
    snprintf(a->id, sizeof(a->id), "%s", g.id);
    snprintf(a->result, sizeof(a->result), "%s", path);
    plat_path(g.source, a->src, sizeof(a->src));
    plat_path(path, a->dst, sizeof(a->dst));
    snprintf(a->tmp, sizeof(a->tmp), "%.*s.moving", (int)sizeof(a->tmp) - 8, a->dst);
    if(g.has_backport) {
      plat_path(g.backport, a->bp_src, sizeof(a->bp_src));
      snprintf(path, sizeof(path), "%s/backports/%s", to, strrchr(g.backport, '/') + 1);
      plat_path(path, a->bp_dst, sizeof(a->bp_dst));
    }
    snprintf(title, sizeof(title), "%s -> %s", g.source, to);
    if((id = job_start("move-game", title, move_job, a, free)) < 0) {
      http_send_error(req, 503, "too many jobs");
    } else {
      api_begin(&w);
      jw_kint(&w, "job", id);
      api_finish(req, &w);
    }
  }
  json_free(b);
}
