/* NeoWEB - save data: what every user has, and safety copies of it.
 *
 * A save is one folder per user and game, with a second one of small files
 * the console shows it with:
 *
 *   /user/home/<user>/savedata_prospero/<TITLE>        (PS5; savedata for PS4)
 *   /user/home/<user>/savedata_prospero_meta/user/<TITLE>
 *
 * A copy keeps those two folders under their own names, grouped by user:
 *
 *   <place>/<Name> (<user>)/<TITLE>/<date-time>/savedata_prospero/...
 *                                              /savedata_prospero_meta/...
 *                                              /info.json
 *
 * <place> is NeoHEN/saves/users on a USB drive, /data/neohen/saves/users on
 * the console, or the root of a ZIP sent to the browser (there without the
 * date-time folder: the ZIP itself is the copy; uploaded again, it is
 * unpacked into the console's copies). The files are the console's own,
 * encrypted for it: a copy goes back to the console it came from.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "fsutil.h"
#include "jobs.h"
#include "log.h"
#include "plat.h"
#include "zipstream.h"

#define HOME        "/user/home"
#define CONSOLE_DIR "/data/neohen/saves/users"
#define USB_DIR     "/NeoHEN/saves/users"
#define MAX_USB     8
#define MAX_ITEMS   4096

typedef struct kind {
  const char *name; /* in the API */
  const char *data; /* folder under the user's home */
  const char *meta;
} kind_t;

static const kind_t KINDS[] = {
  {"ps5", "savedata_prospero", "savedata_prospero_meta"},
  {"ps4", "savedata", "savedata_meta"},
};
#define NKINDS ((int)(sizeof(KINDS) / sizeof(KINDS[0])))

typedef struct item {
  char user[12]; /* 1d31cd9c */
  char title[12];
  const kind_t *kind;
} item_t;

typedef struct item_list {
  item_t *v;
  size_t n, cap;
} item_list_t;

/* ------------------------------------------------------------- helpers */

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

static const kind_t *
kind_named(const char *name) {
  for(int i = 0; i < NKINDS; i++) {
    if(!strcmp(KINDS[i].name, name) || !strcmp(KINDS[i].data, name)) {
      return &KINDS[i];
    }
  }
  return NULL;
}

static int
is_dir(const char *local) {
  struct stat st;
  return !stat(local, &st) && S_ISDIR(st.st_mode);
}

/* The user's name as the console shows it, made safe for a folder name. */
static void
user_name(const char *user, char *out, size_t size) {
  char path[PATH_MAX], local[PATH_MAX], raw[32] = "";
  FILE *f;
  size_t n = 0;

  snprintf(path, sizeof(path), HOME "/%s/username.dat", user);
  plat_path(path, local, sizeof(local));
  if((f = fopen(local, "rb"))) {
    n = fread(raw, 1, sizeof(raw) - 1, f);
    fclose(f);
  }
  raw[n] = 0;
  n = 0;
  for(const char *p = raw; *p && n + 1 < size; p++) {
    out[n++] = isalnum((unsigned char)*p) || strchr("_-.", *p) ? *p : '_';
  }
  out[n] = 0;
  if(!n) {
    snprintf(out, size, "user");
  }
}

/* "Name (1d31cd9c)" */
static void
user_folder(const char *user, char *out, size_t size) {
  char name[40];
  user_name(user, name, sizeof(name));
  snprintf(out, size, "%s (%s)", name, user);
}

/* The folders of a save on the console (console paths). */
static void
save_paths(const item_t *it, char *data, char *meta, size_t size) {
  snprintf(data, size, HOME "/%s/%s/%s", it->user, it->kind->data, it->title);
  snprintf(meta, size, HOME "/%s/%s/user/%s", it->user, it->kind->meta, it->title);
}

static int
list_add(item_list_t *l, const char *user, const kind_t *kind, const char *title) {
  if(l->n == l->cap) {
    size_t cap = l->cap ? l->cap * 2 : 64;
    item_t *v = realloc(l->v, cap * sizeof(item_t));
    if(!v) {
      return -1;
    }
    l->v = v;
    l->cap = cap;
  }
  if(l->n >= MAX_ITEMS) {
    return -1;
  }
  item_t *it = &l->v[l->n++];
  snprintf(it->user, sizeof(it->user), "%s", user);
  snprintf(it->title, sizeof(it->title), "%s", title);
  it->kind = kind;
  return 0;
}

/* Every save of one user (NULL: of every user). */
static void
list_all(item_list_t *l, const char *only_user) {
  char local[PATH_MAX], path[PATH_MAX];
  struct dirent *u, *t;
  DIR *home, *dir;

  plat_path(HOME, local, sizeof(local));
  if(!(home = opendir(local))) {
    return;
  }
  while((u = readdir(home))) {
    if(!is_user_id(u->d_name) || (only_user && strcmp(only_user, u->d_name))) {
      continue;
    }
    for(int k = 0; k < NKINDS; k++) {
      snprintf(path, sizeof(path), HOME "/%s/%s", u->d_name, KINDS[k].data);
      plat_path(path, local, sizeof(local));
      if(!(dir = opendir(local))) {
        continue;
      }
      while((t = readdir(dir))) {
        if(is_title_id(t->d_name)) {
          list_add(l, u->d_name, &KINDS[k], t->d_name);
        }
      }
      closedir(dir);
    }
  }
  closedir(home);
}

/* "user:kind:TITLE" */
static int
parse_item(const char *text, item_list_t *l) {
  char user[16], kind[32], title[16];

  if(sscanf(text, "%15[^:]:%31[^:]:%15s", user, kind, title) != 3 || !is_user_id(user) ||
     !kind_named(kind) || !is_title_id(title)) {
    return -1;
  }
  return list_add(l, user, kind_named(kind), title);
}

/* Is this a drive of its own, not just a folder of the console's disk? */
static int
is_mount(const char *console) {
  char local[PATH_MAX], parent[PATH_MAX];
  struct stat a, b;

  if(getenv("NEOWEB_TEST_USB")) { /* the tests have no drive to plug in */
    plat_path(console, local, sizeof(local));
    return is_dir(local);
  }
  plat_path(console, local, sizeof(local));
  plat_path("/mnt", parent, sizeof(parent));
  return !stat(local, &a) && S_ISDIR(a.st_mode) && !stat(parent, &b) && a.st_dev != b.st_dev;
}

/* Where copies are kept for a destination ("usb0".."usb7", "console"):
 * a console path, or -1 if there is no such place right now. */
static int
dest_base(const char *dest, char *out, size_t size) {
  if(!strcmp(dest, "console")) {
    snprintf(out, size, CONSOLE_DIR);
    return 0;
  }
  if(!strncmp(dest, "usb", 3) && isdigit((unsigned char)dest[3]) && !dest[4] &&
     dest[3] - '0' < MAX_USB) {
    char drive[32];
    snprintf(drive, sizeof(drive), "/mnt/%s", dest);
    if(is_mount(drive)) {
      snprintf(out, size, "%s" USB_DIR, drive);
      return 0;
    }
  }
  return -1;
}

static uint64_t
free_bytes(const char *console) {
  char local[PATH_MAX];
  struct statvfs sv;

  plat_path(console, local, sizeof(local));
  /* the folder may not be there yet: ask about the nearest one that is */
  for(;;) {
    if(!statvfs(local, &sv)) {
      return (uint64_t)sv.f_bavail * sv.f_frsize;
    }
    char *slash = strrchr(local, '/');
    if(!slash || slash == local) {
      return 0;
    }
    *slash = 0;
  }
}

static void
stamp_now(char *out, size_t size) {
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  strftime(out, size, "%Y%m%d-%H%M%S", &tm);
}

/* ---------------------------------------------------------------- list */

void
api_saves_list(http_req_t *req) {
  char local[PATH_MAX], path[PATH_MAX], data[PATH_MAX], meta[PATH_MAX];
  struct dirent *u;
  DIR *home;
  jw_t w;

  api_begin(&w);
  jw_key(&w, "users");
  jw_arr(&w);
  plat_path(HOME, local, sizeof(local));
  if((home = opendir(local))) {
    while((u = readdir(home))) {
      item_list_t l = {0};
      char name[40];

      if(!is_user_id(u->d_name)) {
        continue;
      }
      user_name(u->d_name, name, sizeof(name));
      list_all(&l, u->d_name);
      jw_obj(&w);
      jw_kstr(&w, "id", u->d_name);
      jw_kstr(&w, "name", name);
      jw_key(&w, "saves");
      jw_arr(&w);
      for(size_t i = 0; i < l.n; i++) {
        uint64_t bytes = 0, mbytes = 0;
        uint32_t files = 0, mfiles = 0;
        struct stat st;

        save_paths(&l.v[i], data, meta, sizeof(data));
        plat_path(data, path, sizeof(path));
        fs_tree_size(path, &bytes, &files, NULL);
        jw_obj(&w);
        jw_kstr(&w, "titleId", l.v[i].title);
        jw_kstr(&w, "kind", l.v[i].kind->name);
        jw_kint(&w, "mtime", stat(path, &st) ? 0 : (int64_t)st.st_mtime);
        plat_path(meta, path, sizeof(path));
        fs_tree_size(path, &mbytes, &mfiles, NULL);
        jw_kuint(&w, "bytes", bytes + mbytes);
        jw_kuint(&w, "files", files + mfiles);
        jw_obj_end(&w);
      }
      jw_arr_end(&w);
      jw_obj_end(&w);
      free(l.v);
    }
    closedir(home);
  }
  jw_arr_end(&w);

  /* where a copy can go */
  jw_key(&w, "places");
  jw_arr(&w);
  for(int i = 0; i <= MAX_USB; i++) {
    char dest[16], base[PATH_MAX];
    if(i == MAX_USB) {
      snprintf(dest, sizeof(dest), "console");
    } else {
      snprintf(dest, sizeof(dest), "usb%d", i);
    }
    if(dest_base(dest, base, sizeof(base))) {
      continue;
    }
    jw_obj(&w);
    jw_kstr(&w, "id", dest);
    jw_kstr(&w, "path", base);
    jw_kbool(&w, "usb", i < MAX_USB);
    jw_kuint(&w, "free", free_bytes(base));
    jw_obj_end(&w);
  }
  jw_arr_end(&w);
  api_finish(req, &w);
}

/* -------------------------------------------------------------- backup */

typedef struct backup_arg {
  item_list_t items;
  char base[PATH_MAX]; /* console path */
  char stamp[32];
} backup_arg_t;

static void
backup_free(void *p) {
  backup_arg_t *a = p;
  free(a->items.v);
  free(a);
}

static void
write_info(const char *local_dir, const item_t *it, const char *stamp) {
  char path[PATH_MAX], name[40];
  jw_t w;

  user_name(it->user, name, sizeof(name));
  jw_init(&w);
  jw_obj(&w);
  jw_kstr(&w, "titleId", it->title);
  jw_kstr(&w, "kind", it->kind->name);
  jw_kstr(&w, "user", it->user);
  jw_kstr(&w, "userName", name);
  jw_kstr(&w, "date", stamp);
  jw_kstr(&w, "madeBy", PS5AIO_NAME " " PS5AIO_VERSION);
  jw_obj_end(&w);
  snprintf(path, sizeof(path), "%s/info.json", local_dir);
  if(!w.oom) {
    fs_write_file(path, w.data, w.len);
  }
  jw_free(&w);
}

/* Copies one save into <local_dir>/<its two folder names>. */
static int
copy_save(const item_t *it, const char *local_dir, job_t *job) {
  char data[PATH_MAX], meta[PATH_MAX], src[PATH_MAX], dst[PATH_MAX];

  save_paths(it, data, meta, sizeof(data));
  plat_path(data, src, sizeof(src));
  snprintf(dst, sizeof(dst), "%s/%s", local_dir, it->kind->data);
  if(fs_mkdirs(local_dir, 0777) || fs_copy_tree(src, dst, 0, job)) {
    return -1;
  }
  plat_path(meta, src, sizeof(src));
  if(is_dir(src)) {
    snprintf(dst, sizeof(dst), "%s/%s", local_dir, it->kind->meta);
    if(fs_copy_tree(src, dst, 0, job)) {
      return -1;
    }
  }
  return 0;
}

static void
items_size(const item_list_t *l, uint64_t *bytes, uint32_t *files, job_t *job) {
  char data[PATH_MAX], meta[PATH_MAX], local[PATH_MAX];

  for(size_t i = 0; i < l->n && !(job && job->cancel); i++) {
    save_paths(&l->v[i], data, meta, sizeof(data));
    plat_path(data, local, sizeof(local));
    fs_tree_size(local, bytes, files, job);
    plat_path(meta, local, sizeof(local));
    fs_tree_size(local, bytes, files, job);
  }
}

static int
backup_job(job_t *job, void *p) {
  backup_arg_t *a = p;
  uint64_t bytes = 0;
  uint32_t files = 0;
  size_t done = 0;

  job_set_current(job, "scanning...");
  items_size(&a->items, &bytes, &files, job);
  job->bytes_total = bytes;
  job->files_total = files;
  if(bytes + (16u << 20) > free_bytes(a->base)) {
    return job_fail(job, "not enough free space in %s", a->base);
  }
  for(size_t i = 0; i < a->items.n; i++) {
    const item_t *it = &a->items.v[i];
    char folder[64], dir[PATH_MAX], local[PATH_MAX];

    if(job->cancel) {
      return -1;
    }
    user_folder(it->user, folder, sizeof(folder));
    snprintf(dir, sizeof(dir), "%s/%s/%s/%s", a->base, folder, it->title, a->stamp);
    plat_path(dir, local, sizeof(local));
    if(copy_save(it, local, job)) {
      if(!job->error[0]) {
        job_fail(job, "%s %s: %s", folder, it->title, strerror(errno));
      }
      return -1;
    }
    write_info(local, it, a->stamp);
    done++;
  }
  job->bytes_done = job->bytes_total;
  job->current[0] = 0;
  job_set_result(job, "%zu saves in %s", done, a->base);
  return 0;
}

/* Fills the list from a JSON body: {"items":["user:kind:TITLE",...]},
 * {"user":"1d31cd9c"} or nothing at all for every save there is. */
static int
items_from_body(const jval_t *b, item_list_t *l) {
  const jval_t *items = json_get(b, "items");

  if(items && items->type == J_ARR) {
    for(const jval_t *v = items->child; v; v = v->next) {
      if(v->type != J_STR || parse_item(v->str, l)) {
        return -1;
      }
    }
    return 0;
  }
  const char *user = json_get_str(b, "user", "");
  if(user[0] && !is_user_id(user)) {
    return -1;
  }
  list_all(l, user[0] ? user : NULL);
  return 0;
}

void
api_saves_backup(http_req_t *req) {
  backup_arg_t *a;
  char title[300];
  jval_t *b;
  jw_t w;

  if(!(b = api_body(req))) {
    return;
  }
  if(!(a = calloc(1, sizeof(*a)))) {
    json_free(b);
    http_send_error(req, 500, "out of memory");
    return;
  }
  if(dest_base(json_get_str(b, "dest", ""), a->base, sizeof(a->base))) {
    json_free(b);
    backup_free(a);
    http_send_error(req, 404, "no such place for a copy (is the USB drive in?)");
    return;
  }
  if(items_from_body(b, &a->items) || !a->items.n) {
    json_free(b);
    backup_free(a);
    http_send_error(req, 400, "no saves to copy");
    return;
  }
  json_free(b);
  stamp_now(a->stamp, sizeof(a->stamp));
  snprintf(title, sizeof(title), "Save backup: %zu saves -> %s", a->items.n, a->base);
  int id = job_start("saves", title, backup_job, a, backup_free);
  if(id < 0) {
    http_send_error(req, 503, "too many jobs");
    return;
  }
  api_begin(&w);
  jw_kint(&w, "job", id);
  jw_kstr(&w, "stamp", a->stamp);
  api_finish(req, &w);
}

/* ------------------------------------------------------------- to the PC */

static char *
info_text(const item_t *it, const char *stamp) {
  char name[40];
  jw_t w;

  user_name(it->user, name, sizeof(name));
  jw_init(&w);
  jw_obj(&w);
  jw_kstr(&w, "titleId", it->title);
  jw_kstr(&w, "kind", it->kind->name);
  jw_kstr(&w, "user", it->user);
  jw_kstr(&w, "userName", name);
  jw_kstr(&w, "date", stamp);
  jw_kstr(&w, "madeBy", PS5AIO_NAME " " PS5AIO_VERSION);
  jw_obj_end(&w);
  char *text = w.oom ? NULL : strndup(w.data, w.len);
  jw_free(&w);
  return text;
}

/* GET /api/saves/zip?items=user:kind:TITLE,...  |  ?user=1d31cd9c  |  (all) */
void
api_saves_zip(http_req_t *req) {
  char query[8192], user[16], stamp[32], extra[256];
  item_list_t items = {0};
  zlist_t z = {0};
  int bad = 0;

  if(http_query(req, "items", query, sizeof(query)) && query[0]) {
    for(char *save = NULL, *tok = strtok_r(query, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
      bad |= parse_item(tok, &items);
    }
  } else if(http_query(req, "user", user, sizeof(user)) && user[0]) {
    if(is_user_id(user)) {
      list_all(&items, user);
    } else {
      bad = 1;
    }
  } else {
    list_all(&items, NULL);
  }
  if(bad || !items.n) {
    free(items.v);
    http_send_error(req, 400, "no saves to copy");
    return;
  }

  stamp_now(stamp, sizeof(stamp));
  for(size_t i = 0; i < items.n && !bad; i++) {
    const item_t *it = &items.v[i];
    char folder[64], data[PATH_MAX], meta[PATH_MAX], local[PATH_MAX], name[PATH_MAX];
    char *text;

    user_folder(it->user, folder, sizeof(folder));
    save_paths(it, data, meta, sizeof(data));
    plat_path(data, local, sizeof(local));
    snprintf(name, sizeof(name), "%s/%s/%s", folder, it->title, it->kind->data);
    bad |= zlist_tree(&z, local, name, 0);
    plat_path(meta, local, sizeof(local));
    snprintf(name, sizeof(name), "%s/%s/%s", folder, it->title, it->kind->meta);
    bad |= zlist_tree(&z, local, name, 0);
    snprintf(name, sizeof(name), "%s/%s/info.json", folder, it->title);
    if((text = info_text(it, stamp))) {
      bad |= zlist_add(&z, name, NULL, text, strlen(text), time(NULL));
      free(text);
    }
  }
  free(items.v);
  if(bad) {
    zlist_free(&z);
    http_send_error(req, 500, "out of memory");
    return;
  }

  snprintf(extra, sizeof(extra), "NeoHEN-saves-%s.zip", stamp);
  zip_send(req, &z, extra);
  zlist_free(&z);
}

/* ------------------------------------------------- copies and restoring */

/* "Name (1d31cd9c)" -> "1d31cd9c" */
static int
folder_user(const char *folder, char *user, size_t size) {
  const char *open = strrchr(folder, '(');
  size_t n = strlen(folder);

  if(!open || n < 10 || folder[n - 1] != ')' || (size_t)(folder + n - 1 - (open + 1)) != 8) {
    return -1;
  }
  snprintf(user, size, "%.8s", open + 1);
  return is_user_id(user) ? 0 : -1;
}

static int
plain_name(const char *s) {
  if(!s[0] || s[0] == '.' || strstr(s, "..") || strchr(s, '/') || strchr(s, '\\')) {
    return 0;
  }
  return 1;
}

/* GET /api/saves/backups: every copy kept on a drive or on the console. */
void
api_saves_backups(http_req_t *req) {
  jw_t w;

  api_begin(&w);
  jw_key(&w, "backups");
  jw_arr(&w);
  for(int i = 0; i <= MAX_USB; i++) {
    char dest[16], base[PATH_MAX], local[PATH_MAX];
    struct dirent *u, *t, *s;
    DIR *users, *titles, *stamps;

    if(i == MAX_USB) {
      snprintf(dest, sizeof(dest), "console");
    } else {
      snprintf(dest, sizeof(dest), "usb%d", i);
    }
    if(dest_base(dest, base, sizeof(base))) {
      continue;
    }
    plat_path(base, local, sizeof(local));
    if(!(users = opendir(local))) {
      continue;
    }
    while((u = readdir(users))) {
      char user[16], upath[PATH_MAX];
      if(folder_user(u->d_name, user, sizeof(user))) {
        continue;
      }
      snprintf(upath, sizeof(upath), "%s/%s", local, u->d_name);
      if(!(titles = opendir(upath))) {
        continue;
      }
      while((t = readdir(titles))) {
        char tpath[PATH_MAX];
        if(!is_title_id(t->d_name)) {
          continue;
        }
        snprintf(tpath, sizeof(tpath), "%s/%s", upath, t->d_name);
        if(!(stamps = opendir(tpath))) {
          continue;
        }
        while((s = readdir(stamps))) {
          char spath[PATH_MAX], kpath[PATH_MAX];
          const kind_t *kind = NULL;
          uint64_t bytes = 0;
          uint32_t files = 0;

          if(s->d_name[0] == '.') {
            continue;
          }
          snprintf(spath, sizeof(spath), "%s/%s", tpath, s->d_name);
          for(int k = 0; k < NKINDS && !kind; k++) {
            snprintf(kpath, sizeof(kpath), "%s/%s", spath, KINDS[k].data);
            if(is_dir(kpath)) {
              kind = &KINDS[k];
            }
          }
          if(!kind) {
            continue;
          }
          fs_tree_size(spath, &bytes, &files, NULL);
          jw_obj(&w);
          jw_kstr(&w, "dest", dest);
          jw_kstr(&w, "folder", u->d_name);
          jw_kstr(&w, "user", user);
          jw_kstr(&w, "titleId", t->d_name);
          jw_kstr(&w, "stamp", s->d_name);
          jw_kstr(&w, "kind", kind->name);
          jw_kuint(&w, "bytes", bytes);
          jw_obj_end(&w);
        }
        closedir(stamps);
      }
      closedir(titles);
    }
    closedir(users);
  }
  jw_arr_end(&w);
  api_finish(req, &w);
}

typedef struct restore_arg {
  item_t item;
  char from[PATH_MAX];   /* console path of the copy (the date-time folder) */
  char safety[PATH_MAX]; /* where what is on the console now goes first */
  char stamp[40];
} restore_arg_t;

static int
restore_job(job_t *job, void *p) {
  restore_arg_t *a = p;
  char data[PATH_MAX], meta[PATH_MAX], src[PATH_MAX], dst[PATH_MAX], local[PATH_MAX];
  uint64_t bytes = 0;
  uint32_t files = 0;

  plat_path(a->from, src, sizeof(src));
  fs_tree_size(src, &bytes, &files, job);
  job->bytes_total = bytes;
  job->files_total = files;

  /* what is there now is kept, next to the copy, before anything changes */
  save_paths(&a->item, data, meta, sizeof(data));
  plat_path(data, local, sizeof(local));
  if(is_dir(local)) {
    job_set_current(job, "keeping the current save...");
    plat_path(a->safety, dst, sizeof(dst));
    if(copy_save(&a->item, dst, NULL)) {
      return job_fail(job, "could not keep the current save first: %s", strerror(errno));
    }
    write_info(dst, &a->item, a->stamp);
  }

  snprintf(local, sizeof(local), "%s/%s", a->from, a->item.kind->data);
  plat_path(local, src, sizeof(src));
  plat_path(data, dst, sizeof(dst));
  fs_remove_tree(dst, NULL);
  if(fs_copy_tree(src, dst, 0, job)) {
    if(!job->error[0]) {
      job_fail(job, "%s", strerror(errno));
    }
    return -1;
  }
  snprintf(local, sizeof(local), "%s/%s", a->from, a->item.kind->meta);
  plat_path(local, src, sizeof(src));
  if(is_dir(src)) {
    plat_path(meta, dst, sizeof(dst));
    fs_remove_tree(dst, NULL);
    if(fs_copy_tree(src, dst, 0, job)) {
      if(!job->error[0]) {
        job_fail(job, "%s", strerror(errno));
      }
      return -1;
    }
  }
  job->bytes_done = job->bytes_total;
  job->current[0] = 0;
  job_set_result(job, "%s restored; the save it replaced is in %s", a->item.title, a->safety);
  return 0;
}

/* POST /api/saves/restore {dest, folder, titleId, stamp} */
void
api_saves_restore(http_req_t *req) {
  restore_arg_t *a;
  char base[PATH_MAX], local[PATH_MAX], title[300], tid[16] = "";
  int app_id = 0;
  pid_t pid = -1;
  jval_t *b;
  jw_t w;

  if(!(b = api_body(req))) {
    return;
  }
  const char *folder = json_get_str(b, "folder", "");
  const char *title_id = json_get_str(b, "titleId", "");
  const char *stamp = json_get_str(b, "stamp", "");
  if(!(a = calloc(1, sizeof(*a)))) {
    json_free(b);
    http_send_error(req, 500, "out of memory");
    return;
  }
  if(dest_base(json_get_str(b, "dest", ""), base, sizeof(base)) || !plain_name(folder) ||
     !plain_name(stamp) || !is_title_id(title_id) ||
     folder_user(folder, a->item.user, sizeof(a->item.user))) {
    json_free(b);
    free(a);
    http_send_error(req, 400, "no such copy");
    return;
  }
  snprintf(a->item.title, sizeof(a->item.title), "%s", title_id);
  snprintf(a->from, sizeof(a->from), "%s/%s/%s/%s", base, folder, title_id, stamp);
  json_free(b);
  for(int k = 0; k < NKINDS && !a->item.kind; k++) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", a->from, KINDS[k].data);
    plat_path(path, local, sizeof(local));
    if(is_dir(local)) {
      a->item.kind = &KINDS[k];
    }
  }
  if(!a->item.kind) {
    free(a);
    http_send_error(req, 404, "no such copy");
    return;
  }
  snprintf(title, sizeof(title), HOME "/%s", a->item.user);
  plat_path(title, local, sizeof(local));
  if(!is_dir(local)) {
    free(a);
    http_send_error(req, 409, "that user is not on this console");
    return;
  }
  if(!plat_app_running(tid, sizeof(tid), &app_id, &pid)) {
    free(a);
    http_send_error(req, 409, "a game or app is open: close it first");
    return;
  }
  stamp_now(a->stamp, sizeof(a->stamp));
  strncat(a->stamp, "-pre", sizeof(a->stamp) - strlen(a->stamp) - 1);
  char *slash = strrchr(a->from, '/');
  snprintf(a->safety, sizeof(a->safety), "%.*s/%s", (int)(slash - a->from), a->from, a->stamp);

  snprintf(title, sizeof(title), "Restore save %s (%s)", a->item.title, a->item.user);
  int id = job_start("saves", title, restore_job, a, free);
  if(id < 0) {
    http_send_error(req, 503, "too many jobs");
    return;
  }
  api_begin(&w);
  jw_kint(&w, "job", id);
  api_finish(req, &w);
}

/* -------------------------------------------------- a ZIP coming back
 * A ZIP made by /api/saves/zip, uploaded to the console, is unpacked into
 * the copies kept on the console's disk; from there it is restored like
 * any other. Only what such a ZIP holds is taken: stored (not compressed)
 * files named <Name> (<user>)/<TITLE>/<save folder>/..., each checked
 * against its checksum. */

#define BIG 0xFFFFFFFFull

static uint16_t
get16(const unsigned char *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t
get32(const unsigned char *p) {
  return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static uint64_t
get64(const unsigned char *p) {
  return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static int
read_at(FILE *f, uint64_t off, void *buf, size_t n) {
  return fseeko(f, (off_t)off, SEEK_SET) || fread(buf, 1, n, f) != n ? -1 : 0;
}

/* Where the index of the archive is, and how many files it lists. */
static int
zip_index(FILE *f, uint64_t *dir_off, uint64_t *count) {
  static unsigned char tail[65536 + 22];
  unsigned char loc[20], end64[56];
  uint64_t size;
  size_t n;

  if(fseeko(f, 0, SEEK_END)) {
    return -1;
  }
  size = (uint64_t)ftello(f);
  n = size < sizeof(tail) ? (size_t)size : sizeof(tail);
  if(n < 22 || read_at(f, size - n, tail, n)) {
    return -1;
  }
  for(size_t i = n - 22;; i--) {
    if(get32(tail + i) == 0x06054b50) {
      uint64_t at = size - n + i;
      *count = get16(tail + i + 10);
      *dir_off = get32(tail + i + 16);
      if(at >= 20 && !read_at(f, at - 20, loc, 20) && get32(loc) == 0x07064b50) {
        if(read_at(f, get64(loc + 8), end64, sizeof(end64)) || get32(end64) != 0x06064b50) {
          return -1;
        }
        *count = get64(end64 + 32);
        *dir_off = get64(end64 + 48);
      }
      return 0;
    }
    if(!i) {
      break;
    }
  }
  return -1;
}

typedef struct import_arg {
  char zip[PATH_MAX]; /* console path */
} import_arg_t;

/* <Name> (<user>)/<TITLE>/<save folder or info.json>[/...] -> where it goes
 * below <base>/<Name> (<user>)/<TITLE>/<stamp>/. 0 if the name is one of ours. */
static int
import_target(const char *name, const char *base, const char *stamp, char *out, size_t size) {
  char folder[80], title[16], user[16];
  const char *a = strchr(name, '/'), *b = a ? strchr(a + 1, '/') : NULL, *rest;
  int ok = 0;

  if(!a || !b || (size_t)(a - name) >= sizeof(folder) || b - a - 1 != 9 || strstr(name, "..") ||
     strchr(name, '\\') || name[strlen(name) - 1] == '/') {
    return -1;
  }
  snprintf(folder, sizeof(folder), "%.*s", (int)(a - name), name);
  snprintf(title, sizeof(title), "%.9s", a + 1);
  rest = b + 1;
  if(folder_user(folder, user, sizeof(user)) || !plain_name(folder) || !is_title_id(title)) {
    return -1;
  }
  if(!strcmp(rest, "info.json")) {
    ok = 1;
  }
  for(int k = 0; k < NKINDS && !ok; k++) {
    size_t d = strlen(KINDS[k].data), m = strlen(KINDS[k].meta);
    ok = (!strncmp(rest, KINDS[k].data, d) && rest[d] == '/') ||
         (!strncmp(rest, KINDS[k].meta, m) && rest[m] == '/');
  }
  if(!ok) {
    return -1;
  }
  snprintf(out, size, "%s/%s/%s/%s/%s", base, folder, title, stamp, rest);
  return 0;
}

static int
import_job(job_t *job, void *p) {
  static unsigned char buf[256 * 1024];
  static pthread_mutex_t one = PTHREAD_MUTEX_INITIALIZER; /* buf and the tail */
  import_arg_t *a = p;
  char local[PATH_MAX], stamp[40] = "";
  unsigned char *dir = NULL;
  uint64_t dir_off = 0, count = 0, dir_len = 0, total = 0, at;
  size_t taken = 0;
  int r = 0;
  FILE *f;

  plat_path(a->zip, local, sizeof(local));
  if(!(f = fopen(local, "rb"))) {
    return job_fail(job, "cannot open the ZIP: %s", strerror(errno));
  }
  pthread_mutex_lock(&one);
  if(zip_index(f, &dir_off, &count) || fseeko(f, 0, SEEK_END) ||
     dir_off >= (uint64_t)ftello(f)) {
    r = job_fail(job, "this is not a ZIP of saves");
  }
  if(!r) {
    dir_len = (uint64_t)ftello(f) - dir_off;
    if(dir_len > (64u << 20) || !(dir = malloc((size_t)dir_len)) ||
       read_at(f, dir_off, dir, (size_t)dir_len)) {
      r = job_fail(job, "the index of the ZIP cannot be read");
    }
  }

  /* first pass: what there is, how much, and when the copy was made (the
   * time kept with its info.json) */
  at = 0;
  for(uint64_t i = 0; !r && i < count && at + 46 <= dir_len && get32(dir + at) == 0x02014b50; i++) {
    uint16_t nlen = get16(dir + at + 28), xlen = get16(dir + at + 30), clen = get16(dir + at + 32);
    uint64_t usize = get32(dir + at + 24);
    char name[PATH_MAX], target[PATH_MAX];

    snprintf(name, sizeof(name), "%.*s", (int)nlen, dir + at + 46);
    if(usize == BIG && xlen >= 12) {
      usize = get64(dir + at + 46 + nlen + 4);
    }
    if(!import_target(name, "", "x", target, sizeof(target))) {
      if(get16(dir + at + 10) != 0) {
        r = job_fail(job, "the ZIP is compressed: use it as NeoWEB made it");
        break;
      }
      total += usize;
      if(!stamp[0] && strlen(name) > 9 && !strcmp(name + strlen(name) - 9, "info.json")) {
        uint16_t t = get16(dir + at + 12), d = get16(dir + at + 14);
        snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d-pc", 1980 + (d >> 9),
                 (d >> 5) & 15, d & 31, t >> 11, (t >> 5) & 63, (t & 31) * 2);
      }
    }
    at += 46u + nlen + xlen + clen;
  }
  if(!r && !total) {
    r = job_fail(job, "there are no saves in this ZIP");
  }
  if(!r && !stamp[0]) {
    stamp_now(stamp, sizeof(stamp) - 4);
    strcat(stamp, "-pc");
  }
  job->bytes_total = total;
  if(!r && total + (16u << 20) > free_bytes(CONSOLE_DIR)) {
    r = job_fail(job, "not enough free space on the console");
  }

  at = 0;
  for(uint64_t i = 0; !r && i < count && at + 46 <= dir_len && get32(dir + at) == 0x02014b50; i++) {
    uint16_t nlen = get16(dir + at + 28), xlen = get16(dir + at + 30), clen = get16(dir + at + 32);
    uint32_t crc = get32(dir + at + 16), sum = 0;
    uint64_t usize = get32(dir + at + 24), off = get32(dir + at + 42);
    const unsigned char *x = dir + at + 46 + nlen;
    char name[PATH_MAX], target[PATH_MAX], out_local[PATH_MAX];
    unsigned char lh[30];
    FILE *out;

    snprintf(name, sizeof(name), "%.*s", (int)nlen, dir + at + 46);
    /* ZIP64: the real numbers follow, in this order, for each that is all ones */
    if(xlen >= 4 && get16(x) == 1) {
      const unsigned char *v = x + 4;
      if(get32(dir + at + 24) == BIG) {
        usize = get64(v);
        v += 8;
      }
      if(get32(dir + at + 20) == BIG) {
        v += 8;
      }
      if(get32(dir + at + 42) == BIG) {
        off = get64(v);
      }
    }
    at += 46u + nlen + xlen + clen;
    if(import_target(name, CONSOLE_DIR, stamp, target, sizeof(target))) {
      continue; /* not one of ours: left alone */
    }
    if(job->cancel) {
      r = -1;
      break;
    }
    plat_path(target, out_local, sizeof(out_local));
    job_set_current(job, "%s", name);
    errno = 0;
    if(read_at(f, off, lh, sizeof(lh)) || get32(lh) != 0x04034b50 ||
       fseeko(f, (off_t)(off + 30 + get16(lh + 26) + get16(lh + 28)), SEEK_SET) ||
       fs_mkparents(out_local) || !(out = fopen(out_local, "wb"))) {
      r = job_fail(job, "%s: %s", name, errno ? strerror(errno) : "bad entry");
      break;
    }
    for(uint64_t left = usize; left && !r;) {
      size_t want = left < sizeof(buf) ? (size_t)left : sizeof(buf);
      if(fread(buf, 1, want, f) != want || fwrite(buf, 1, want, out) != want) {
        r = job_fail(job, "%s: the ZIP is cut short, or the disk is full", name);
        break;
      }
      sum = zip_crc32(sum, buf, want);
      left -= want;
      job->bytes_done += want;
    }
    if(fclose(out) && !r) {
      r = job_fail(job, "%s: %s", name, strerror(errno));
    }
    if(!r && sum != crc) {
      r = job_fail(job, "%s: damaged (checksum does not match)", name);
    }
    if(!r) {
      taken++;
      job->files_done++;
    }
  }
  pthread_mutex_unlock(&one);
  free(dir);
  fclose(f);
  if(r) {
    return -1;
  }
  unlink(local); /* it is unpacked: the upload is not needed any more */
  job->current[0] = 0;
  job_set_result(job, "%zu files unpacked as %s", taken, stamp);
  return 0;
}

/* POST /api/saves/import {path}: a ZIP already uploaded to the console. */
void
api_saves_import(http_req_t *req) {
  import_arg_t *a;
  char local[PATH_MAX];
  jval_t *b;
  jw_t w;

  if(!(b = api_body(req))) {
    return;
  }
  if(!(a = calloc(1, sizeof(*a)))) {
    json_free(b);
    http_send_error(req, 500, "out of memory");
    return;
  }
  if(api_body_path(req, b, "path", local, sizeof(local))) {
    json_free(b);
    free(a);
    return;
  }
  snprintf(a->zip, sizeof(a->zip), "%s", json_get_str(b, "path", ""));
  json_free(b);
  int id = job_start("saves", "Unpack a ZIP of saves", import_job, a, free);
  if(id < 0) {
    http_send_error(req, 503, "too many jobs");
    return;
  }
  api_begin(&w);
  jw_kint(&w, "job", id);
  api_finish(req, &w);
}
