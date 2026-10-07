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
 * date-time folder: the ZIP itself is the copy). The files are the console's
 * own, encrypted for it: a copy goes back to the console it came from.
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

/* ----------------------------------------------------------------- zip
 * Not compressed (the files are encrypted, they would not shrink), with
 * ZIP64 fields where a size or an offset needs them. Every size is known
 * before the first byte goes out, so the browser gets a Content-Length;
 * each file is read twice, once for its checksum. */

typedef struct zentry {
  char    *name;  /* inside the archive */
  char    *local; /* on disk; NULL for text held in `text` */
  char    *text;
  uint64_t size;
  uint64_t offset;
  uint32_t crc;
  uint16_t dos_time, dos_date;
} zentry_t;

typedef struct zlist {
  zentry_t *v;
  size_t n, cap;
} zlist_t;

static uint32_t g_crc_table[256];

static uint32_t
crc32_update(uint32_t crc, const unsigned char *p, size_t n) {
  if(!g_crc_table[1]) {
    for(uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for(int k = 0; k < 8; k++) {
        c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      g_crc_table[i] = c;
    }
  }
  crc = ~crc;
  while(n--) {
    crc = g_crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  }
  return ~crc;
}

static void
zlist_free(zlist_t *z) {
  for(size_t i = 0; i < z->n; i++) {
    free(z->v[i].name);
    free(z->v[i].local);
    free(z->v[i].text);
  }
  free(z->v);
}

static int
zlist_add(zlist_t *z, const char *name, const char *local, const char *text, uint64_t size,
          time_t mtime) {
  struct tm tm;

  if(z->n == z->cap) {
    size_t cap = z->cap ? z->cap * 2 : 256;
    zentry_t *v = realloc(z->v, cap * sizeof(zentry_t));
    if(!v) {
      return -1;
    }
    z->v = v;
    z->cap = cap;
  }
  zentry_t *e = &z->v[z->n];
  memset(e, 0, sizeof(*e));
  e->name = strdup(name);
  e->local = local ? strdup(local) : NULL;
  e->text = text ? strdup(text) : NULL;
  e->size = size;
  localtime_r(&mtime, &tm);
  if(tm.tm_year < 80) {
    tm.tm_year = 80;
    tm.tm_mon = 0;
    tm.tm_mday = 1;
  }
  e->dos_time = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
  e->dos_date = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
  if(!e->name || (local && !e->local) || (text && !e->text)) {
    return -1;
  }
  z->n++;
  return 0;
}

/* Every file below a folder, as <prefix>/<path below it>. */
static int
zlist_tree(zlist_t *z, const char *local, const char *prefix, int depth) {
  struct dirent *e;
  DIR *dir = opendir(local);
  int r = 0;

  if(!dir) {
    return 0;
  }
  while(!r && (e = readdir(dir))) {
    char path[PATH_MAX], name[PATH_MAX];
    struct stat st;

    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", local, e->d_name);
    snprintf(name, sizeof(name), "%s/%s", prefix, e->d_name);
    if(lstat(path, &st)) {
      continue;
    }
    if(S_ISDIR(st.st_mode)) {
      if(depth < 16) {
        r = zlist_tree(z, path, name, depth + 1);
      }
    } else if(S_ISREG(st.st_mode)) {
      r = zlist_add(z, name, path, NULL, (uint64_t)st.st_size, st.st_mtime);
    }
  }
  closedir(dir);
  return r;
}

static void
put16(unsigned char **p, uint16_t v) {
  *(*p)++ = (unsigned char)v;
  *(*p)++ = (unsigned char)(v >> 8);
}

static void
put32(unsigned char **p, uint32_t v) {
  put16(p, (uint16_t)v);
  put16(p, (uint16_t)(v >> 16));
}

static void
put64(unsigned char **p, uint64_t v) {
  put32(p, (uint32_t)v);
  put32(p, (uint32_t)(v >> 32));
}

#define BIG 0xFFFFFFFFull

/* The header that goes before a file's bytes. Returns its length. */
static size_t
zip_local(const zentry_t *e, unsigned char *out) {
  unsigned char *p = out;
  size_t nlen = strlen(e->name);
  int big = e->size >= BIG;

  put32(&p, 0x04034b50);
  put16(&p, big ? 45 : 20);
  put16(&p, 0x0800); /* names are UTF-8 */
  put16(&p, 0);      /* stored */
  put16(&p, e->dos_time);
  put16(&p, e->dos_date);
  put32(&p, e->crc);
  put32(&p, big ? (uint32_t)BIG : (uint32_t)e->size);
  put32(&p, big ? (uint32_t)BIG : (uint32_t)e->size);
  put16(&p, (uint16_t)nlen);
  put16(&p, big ? 20 : 0);
  memcpy(p, e->name, nlen);
  p += nlen;
  if(big) {
    put16(&p, 1);
    put16(&p, 16);
    put64(&p, e->size);
    put64(&p, e->size);
  }
  return (size_t)(p - out);
}

/* Its line in the index at the end. */
static size_t
zip_central(const zentry_t *e, unsigned char *out) {
  unsigned char *p = out;
  size_t nlen = strlen(e->name);
  int big_size = e->size >= BIG, big_off = e->offset >= BIG;
  uint16_t extra = (uint16_t)((big_size ? 16 : 0) + (big_off ? 8 : 0));

  put32(&p, 0x02014b50);
  put16(&p, 45);
  put16(&p, extra ? 45 : 20);
  put16(&p, 0x0800);
  put16(&p, 0);
  put16(&p, e->dos_time);
  put16(&p, e->dos_date);
  put32(&p, e->crc);
  put32(&p, big_size ? (uint32_t)BIG : (uint32_t)e->size);
  put32(&p, big_size ? (uint32_t)BIG : (uint32_t)e->size);
  put16(&p, (uint16_t)nlen);
  put16(&p, extra ? (uint16_t)(extra + 4) : 0);
  put16(&p, 0);
  put16(&p, 0);
  put16(&p, 0);
  put32(&p, 0);
  put32(&p, big_off ? (uint32_t)BIG : (uint32_t)e->offset);
  memcpy(p, e->name, nlen);
  p += nlen;
  if(extra) {
    put16(&p, 1);
    put16(&p, extra);
    if(big_size) {
      put64(&p, e->size);
      put64(&p, e->size);
    }
    if(big_off) {
      put64(&p, e->offset);
    }
  }
  return (size_t)(p - out);
}

static size_t
zip_end(size_t count, uint64_t dir_off, uint64_t dir_size, unsigned char *out) {
  unsigned char *p = out;
  int big = count >= 0xFFFF || dir_off >= BIG || dir_size >= BIG;

  if(big) {
    put32(&p, 0x06064b50);
    put64(&p, 44);
    put16(&p, 45);
    put16(&p, 45);
    put32(&p, 0);
    put32(&p, 0);
    put64(&p, count);
    put64(&p, count);
    put64(&p, dir_size);
    put64(&p, dir_off);
    put32(&p, 0x07064b50);
    put32(&p, 0);
    put64(&p, dir_off + dir_size);
    put32(&p, 1);
  }
  put32(&p, 0x06054b50);
  put16(&p, 0);
  put16(&p, 0);
  put16(&p, count >= 0xFFFF ? 0xFFFF : (uint16_t)count);
  put16(&p, count >= 0xFFFF ? 0xFFFF : (uint16_t)count);
  put32(&p, dir_size >= BIG ? (uint32_t)BIG : (uint32_t)dir_size);
  put32(&p, dir_off >= BIG ? (uint32_t)BIG : (uint32_t)dir_off);
  put16(&p, 0);
  return (size_t)(p - out);
}

/* Reads exactly e->size bytes of the file (zeros past its end, should it
 * have shrunk since it was measured), to the client or just into the
 * checksum. */
static int
zip_pass(zentry_t *e, int fd, unsigned char *buf, size_t bufsize) {
  uint64_t left = e->size;
  uint32_t crc = 0;
  FILE *f;

  if(!e->local) {
    crc = crc32_update(0, (const unsigned char *)e->text, (size_t)e->size);
    if(fd >= 0 && http_send_all(fd, e->text, (size_t)e->size)) {
      return -1;
    }
    e->crc = crc;
    return 0;
  }
  f = fopen(e->local, "rb");
  while(left) {
    size_t want = left < bufsize ? (size_t)left : bufsize;
    size_t got = f ? fread(buf, 1, want, f) : 0;
    if(got < want) {
      memset(buf + got, 0, want - got);
    }
    crc = crc32_update(crc, buf, want);
    if(fd >= 0 && http_send_all(fd, buf, want)) {
      if(f) {
        fclose(f);
      }
      return -1;
    }
    left -= want;
  }
  if(f) {
    fclose(f);
  }
  if(fd < 0) {
    e->crc = crc;
  }
  return 0;
}

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
  static unsigned char buf[256 * 1024];
  static pthread_mutex_t one = PTHREAD_MUTEX_INITIALIZER; /* buf is shared */
  unsigned char head[PATH_MAX + 128];
  char query[8192], user[16], stamp[32], extra[256];
  item_list_t items = {0};
  zlist_t z = {0};
  uint64_t offset = 0, dir_size = 0;
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

  /* lay the archive out: every length is known from here */
  for(size_t i = 0; i < z.n; i++) {
    z.v[i].offset = offset;
    offset += zip_local(&z.v[i], head) + z.v[i].size;
  }
  for(size_t i = 0; i < z.n; i++) {
    dir_size += zip_central(&z.v[i], head);
  }
  uint64_t total = offset + dir_size + zip_end(z.n, offset, dir_size, head);

  snprintf(extra, sizeof(extra),
           "Content-Disposition: attachment; filename=\"NeoHEN-saves-%s.zip\"\r\n", stamp);
  http_send_head(req, 200, "application/zip", (int64_t)total, extra);
  req->keep_alive = 0; /* a file that fails half way ends the connection */

  pthread_mutex_lock(&one);
  int failed = 0;
  for(size_t i = 0; i < z.n && !failed; i++) {
    zip_pass(&z.v[i], -1, buf, sizeof(buf));
    size_t n = zip_local(&z.v[i], head);
    failed = http_send_all(req->fd, head, n) || zip_pass(&z.v[i], req->fd, buf, sizeof(buf));
  }
  for(size_t i = 0; i < z.n && !failed; i++) {
    size_t n = zip_central(&z.v[i], head);
    failed = http_send_all(req->fd, head, n);
  }
  if(!failed) {
    size_t n = zip_end(z.n, offset, dir_size, head);
    http_send_all(req->fd, head, n);
  }
  pthread_mutex_unlock(&one);
  LOG_INFO("saves zip: %zu files, %llu bytes%s", z.n, (unsigned long long)total,
           failed ? " (cut short)" : "");
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
