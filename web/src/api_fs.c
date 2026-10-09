/* PS5AIO - file manager endpoints.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "api.h"
#include <time.h>
#include <ctype.h>
#include "fsutil.h"
#include "sha256.h"
#include "jobs.h"
#include "log.h"
#include "plat.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define UPLOAD_BUF (4 * 1024 * 1024)

static const char *
type_name(mode_t m) {
  if(S_ISDIR(m)) return "dir";
  if(S_ISREG(m)) return "file";
  if(S_ISLNK(m)) return "link";
  return "other";
}

typedef struct dent {
  char    *name;
  int      is_dir;
  struct stat st;
  int      lnk;
} dent_t;

static int
dent_cmp(const void *a, const void *b) {
  const dent_t *x = a, *y = b;
  if(x->is_dir != y->is_dir) {
    return y->is_dir - x->is_dir;
  }
  return strcasecmp(x->name, y->name);
}

void
api_fs_list(http_req_t *req) {
  char console[PATH_MAX], local[PATH_MAX];
  dent_t *ents = NULL;
  size_t n = 0, cap = 0;
  DIR *d;
  struct dirent *e;
  jw_t w;

  if(api_query_path(req, "path", console, sizeof(console), local, sizeof(local))) {
    return;
  }
  if(!(d = opendir(local))) {
    http_send_error(req, errno == ENOENT ? 404 : 403, "%s: %s", console, strerror(errno));
    return;
  }
  while((e = readdir(d))) {
    char child[PATH_MAX + 256];
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    if(n == cap) {
      cap = cap ? cap * 2 : 64;
      dent_t *p = realloc(ents, cap * sizeof(dent_t));
      if(!p) {
        break;
      }
      ents = p;
    }
    dent_t *de = &ents[n];
    memset(de, 0, sizeof(*de));
    snprintf(child, sizeof(child), "%s/%s", local, e->d_name);
    if(lstat(child, &de->st)) {
      memset(&de->st, 0, sizeof(de->st));
    }
    if(S_ISLNK(de->st.st_mode)) {
      struct stat tgt;
      de->lnk = 1;
      if(!stat(child, &tgt)) {
        de->st = tgt;
      }
    }
    de->is_dir = S_ISDIR(de->st.st_mode);
    if(!(de->name = strdup(e->d_name))) {
      break;
    }
    n++;
  }
  closedir(d);
  qsort(ents, n, sizeof(dent_t), dent_cmp);

  api_begin(&w);
  jw_kstr(&w, "path", console);
  jw_key(&w, "entries");
  jw_arr(&w);
  for(size_t i = 0; i < n; i++) {
    jw_obj(&w);
    jw_kstr(&w, "name", ents[i].name);
    jw_kstr(&w, "type", type_name(ents[i].st.st_mode));
    jw_kbool(&w, "link", ents[i].lnk);
    jw_kuint(&w, "size", (uint64_t)ents[i].st.st_size);
    jw_kint(&w, "mtime", (int64_t)ents[i].st.st_mtime);
    jw_kint(&w, "mode", ents[i].st.st_mode & 07777);
    jw_obj_end(&w);
    free(ents[i].name);
  }
  jw_arr_end(&w);
  free(ents);
  api_finish(req, &w);
}

void
api_fs_stat(http_req_t *req) {
  char console[PATH_MAX], local[PATH_MAX];
  struct stat st;
  jw_t w;

  if(api_query_path(req, "path", console, sizeof(console), local, sizeof(local))) {
    return;
  }
  api_begin(&w);
  jw_kstr(&w, "path", console);
  if(stat(local, &st)) {
    jw_kbool(&w, "exists", 0);
  } else {
    jw_kbool(&w, "exists", 1);
    jw_kstr(&w, "type", type_name(st.st_mode));
    jw_kuint(&w, "size", (uint64_t)st.st_size);
    jw_kint(&w, "mtime", (int64_t)st.st_mtime);
    jw_kint(&w, "mode", st.st_mode & 07777);
  }
  api_finish(req, &w);
}

/* recursive listing of regular files (used by the PC for folder downloads) */
static void
walk(jw_t *w, const char *local, const char *rel, size_t *count, size_t max) {
  DIR *d = opendir(local);
  struct dirent *e;
  if(!d) {
    return;
  }
  while((e = readdir(d)) && *count < max) {
    char lchild[PATH_MAX], rchild[PATH_MAX];
    struct stat st;
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    snprintf(lchild, sizeof(lchild), "%s/%s", local, e->d_name);
    snprintf(rchild, sizeof(rchild), "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
    if(lstat(lchild, &st)) {
      continue;
    }
    if(S_ISDIR(st.st_mode)) {
      jw_obj(w);
      jw_kstr(w, "path", rchild);
      jw_kstr(w, "type", "dir");
      jw_obj_end(w);
      (*count)++;
      walk(w, lchild, rchild, count, max);
    } else if(S_ISREG(st.st_mode)) {
      jw_obj(w);
      jw_kstr(w, "path", rchild);
      jw_kstr(w, "type", "file");
      jw_kuint(w, "size", (uint64_t)st.st_size);
      jw_kint(w, "mtime", (int64_t)st.st_mtime);
      jw_obj_end(w);
      (*count)++;
    }
  }
  closedir(d);
}

void
api_fs_walk(http_req_t *req) {
  char console[PATH_MAX], local[PATH_MAX];
  size_t count = 0;
  size_t max = (size_t)http_query_int(req, "max", 200000);
  jw_t w;

  if(api_query_path(req, "path", console, sizeof(console), local, sizeof(local))) {
    return;
  }
  api_begin(&w);
  jw_kstr(&w, "path", console);
  jw_key(&w, "entries");
  jw_arr(&w);
  walk(&w, local, "", &count, max);
  jw_arr_end(&w);
  jw_kbool(&w, "truncated", count >= max);
  api_finish(req, &w);
}

/* ---------------------------------------------------------------- search
 * Files and folders below a folder whose name has the text in it, in any
 * case. It stops at `max` results, after a few seconds or after a great
 * many entries, and says so. The views other programs have of the same
 * files (/mnt/sandbox) and what is not a disk (/dev, /proc) are left out. */
typedef struct search {
  jw_t *w;
  const char *needle; /* lower case */
  size_t found, max;
  uint64_t seen;
  time_t until;
  int cut;
} search_t;

static int
name_has(const char *name, const char *needle) {
  size_t n = strlen(needle);

  for(const char *p = name; *p; p++) {
    size_t i = 0;
    while(i < n && p[i] && tolower((unsigned char)p[i]) == needle[i]) {
      i++;
    }
    if(i == n) {
      return 1;
    }
  }
  return 0;
}

static void
search_dir(search_t *s, const char *local, const char *console, int depth) {
  DIR *d;
  struct dirent *e;

  if(depth > 24 || !(d = opendir(local))) {
    return;
  }
  while(!s->cut && (e = readdir(d))) {
    char lchild[PATH_MAX], cchild[PATH_MAX];
    struct stat st;

    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    if(s->found >= s->max || ++s->seen > 400000 || (!(s->seen & 255) && time(NULL) > s->until)) {
      s->cut = 1;
      break;
    }
    snprintf(lchild, sizeof(lchild), "%s/%s", local, e->d_name);
    snprintf(cchild, sizeof(cchild), "%s%s%s", console, strcmp(console, "/") ? "/" : "", e->d_name);
    if(lstat(lchild, &st)) {
      continue;
    }
    if(name_has(e->d_name, s->needle)) {
      jw_obj(s->w);
      jw_kstr(s->w, "path", cchild);
      jw_kstr(s->w, "type", S_ISDIR(st.st_mode) ? "dir" : S_ISREG(st.st_mode) ? "file" : "other");
      jw_kuint(s->w, "size", S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0);
      jw_kint(s->w, "mtime", (int64_t)st.st_mtime);
      jw_obj_end(s->w);
      s->found++;
    }
    if(S_ISDIR(st.st_mode) && strcmp(cchild, "/dev") && strcmp(cchild, "/proc") &&
       strcmp(cchild, "/mnt/sandbox")) {
      search_dir(s, lchild, cchild, depth + 1);
    }
  }
  closedir(d);
}

/* GET /api/fs/search?path=/data&q=text[&max=300] */
void
api_fs_search(http_req_t *req) {
  char console[PATH_MAX], local[PATH_MAX], q[128] = "";
  search_t s = {0};
  jw_t w;

  if(api_query_path(req, "path", console, sizeof(console), local, sizeof(local))) {
    return;
  }
  http_query(req, "q", q, sizeof(q));
  if(strlen(q) < 2) {
    http_send_error(req, 400, "at least two characters to look for");
    return;
  }
  for(char *p = q; *p; p++) {
    *p = (char)tolower((unsigned char)*p);
  }
  s.w = &w;
  s.needle = q;
  s.max = (size_t)http_query_int(req, "max", 300);
  if(s.max < 1 || s.max > 5000) {
    s.max = 300;
  }
  s.until = time(NULL) + 8;
  api_begin(&w);
  jw_kstr(&w, "path", console);
  jw_key(&w, "entries");
  jw_arr(&w);
  search_dir(&s, local, console, 0);
  jw_arr_end(&w);
  jw_kbool(&w, "truncated", s.cut);
  jw_kuint(&w, "scanned", s.seen);
  api_finish(req, &w);
}

void
api_fs_size(http_req_t *req) {
  char console[PATH_MAX], local[PATH_MAX];
  uint64_t bytes = 0;
  uint32_t files = 0;
  jw_t w;

  if(api_query_path(req, "path", console, sizeof(console), local, sizeof(local))) {
    return;
  }
  if(fs_tree_size(local, &bytes, &files, NULL)) {
    http_send_error(req, 404, "%s: %s", console, strerror(errno));
    return;
  }
  api_begin(&w);
  jw_kstr(&w, "path", console);
  jw_kuint(&w, "bytes", bytes);
  jw_kuint(&w, "files", files);
  api_finish(req, &w);
}

void
api_fs_read_text(http_req_t *req) {
  char console[PATH_MAX], local[PATH_MAX];
  char *data = NULL;
  size_t len = 0;
  jw_t w;

  if(api_query_path(req, "path", console, sizeof(console), local, sizeof(local))) {
    return;
  }
  if(fs_read_file(local, &data, &len, 8 * 1024 * 1024)) {
    http_send_error(req, errno == ENOENT ? 404 : 500, "%s: %s", console, strerror(errno));
    return;
  }
  api_begin(&w);
  jw_kstr(&w, "path", console);
  jw_key(&w, "content");
  jw_strn(&w, data, len);
  free(data);
  api_finish(req, &w);
}

void
api_fs_write_text(http_req_t *req) {
  char local[PATH_MAX];
  jval_t *b = api_body(req);
  if(!b) {
    return;
  }
  if(api_body_path(req, b, "path", local, sizeof(local))) {
    json_free(b);
    return;
  }
  jval_t *c = json_get(b, "content");
  if(!c || c->type != J_STR) {
    json_free(b);
    http_send_error(req, 400, "missing 'content'");
    return;
  }
  int r = fs_write_file(local, c->str, strlen(c->str));
  json_free(b);
  if(r) {
    http_send_error(req, 500, "write failed: %s", strerror(errno));
    return;
  }
  http_send_ok(req);
}

void
api_fs_mkdir(http_req_t *req) {
  char local[PATH_MAX];
  jval_t *b = api_body(req);
  if(!b) {
    return;
  }
  if(!api_body_path(req, b, "path", local, sizeof(local))) {
    if(fs_mkdirs(local, 0777)) {
      http_send_error(req, 500, "mkdir: %s", strerror(errno));
    } else {
      http_send_ok(req);
    }
  }
  json_free(b);
}

void
api_fs_delete(http_req_t *req) {
  char local[PATH_MAX];
  jval_t *b = api_body(req);
  if(!b) {
    return;
  }
  const char *p = json_get_str(b, "path", "");
  if(!strcmp(p, "/") || !strcmp(p, "/user") || !strcmp(p, "/system") ||
     !strcmp(p, "/system_ex") || !strcmp(p, "/data")) {
    http_send_error(req, 403, "refusing to delete %s", p);
    json_free(b);
    return;
  }
  if(!api_body_path(req, b, "path", local, sizeof(local))) {
    if(fs_remove_tree(local, NULL)) {
      http_send_error(req, 500, "delete: %s", strerror(errno));
    } else {
      http_send_ok(req);
    }
  }
  json_free(b);
}

void
api_fs_rename(http_req_t *req) {
  char from[PATH_MAX], to[PATH_MAX];
  jval_t *b = api_body(req);
  if(!b) {
    return;
  }
  if(!api_body_path(req, b, "from", from, sizeof(from)) &&
     !api_body_path(req, b, "to", to, sizeof(to))) {
    fs_mkparents(to);
    if(rename(from, to)) {
      http_send_error(req, 500, "rename: %s", strerror(errno));
    } else {
      http_send_ok(req);
    }
  }
  json_free(b);
}

void
api_fs_chmod(http_req_t *req) {
  char local[PATH_MAX];
  jval_t *b = api_body(req);
  if(!b) {
    return;
  }
  if(!api_body_path(req, b, "path", local, sizeof(local))) {
    const char *ms = json_get_str(b, "mode", "0777");
    mode_t mode = (mode_t)strtol(ms, NULL, 8);
    if(chmod(local, mode)) {
      http_send_error(req, 500, "chmod: %s", strerror(errno));
    } else {
      http_send_ok(req);
    }
  }
  json_free(b);
}

/* ------------------------------------------------------------ copy job */

typedef struct copy_arg {
  char src[PATH_MAX];
  char dst[PATH_MAX];
  int  move;
  int  flags;
} copy_arg_t;

static int
copy_job(job_t *job, void *p) {
  copy_arg_t *a = p;
  uint64_t bytes = 0;
  uint32_t files = 0;

  job_set_current(job, "scanning...");
  fs_tree_size(a->src, &bytes, &files, job);
  job->bytes_total = bytes;
  job->files_total = files;

  if(a->move) {
    if(fs_move(a->src, a->dst, job)) {
      return -1;
    }
  } else if(fs_copy_tree(a->src, a->dst, a->flags, job)) {
    return -1;
  }
  job->bytes_done = job->bytes_total;
  job->current[0] = 0;
  return 0;
}

void
api_fs_copy(http_req_t *req) {
  copy_arg_t *a = calloc(1, sizeof(copy_arg_t));
  char title[600];
  jw_t w;
  jval_t *b;

  if(!a) {
    http_send_error(req, 500, "out of memory");
    return;
  }
  if(!(b = api_body(req))) {
    free(a);
    return;
  }
  if(api_body_path(req, b, "from", a->src, sizeof(a->src)) ||
     api_body_path(req, b, "to", a->dst, sizeof(a->dst))) {
    json_free(b);
    free(a);
    return;
  }
  a->move = json_get_bool(b, "move", 0);
  if(json_get_bool(b, "skipErrors", 0)) {
    a->flags |= FS_COPY_SKIP_ERRORS;
  }
  if(!json_get_bool(b, "overwrite", 1)) {
    a->flags |= FS_COPY_NO_OVERWRITE;
  }
  snprintf(title, sizeof(title), "%s %s -> %s", a->move ? "Move" : "Copy",
           json_get_str(b, "from", ""), json_get_str(b, "to", ""));
  json_free(b);

  int id = job_start(a->move ? "move" : "copy", title, copy_job, a, free);
  if(id < 0) {
    http_send_error(req, 503, "too many jobs");
    return;
  }
  api_begin(&w);
  jw_kint(&w, "job", id);
  api_finish(req, &w);
}

/* ------------------------------------------------------------ transfer */

void
api_fs_get(http_req_t *req) {
  char console[PATH_MAX], local[PATH_MAX];
  if(api_query_path(req, "path", console, sizeof(console), local, sizeof(local))) {
    return;
  }
  http_send_file(req, local, (int)http_query_int(req, "download", 0));
}

/* ------------------------------------------------------------------ hash */

typedef struct hash_arg {
  char path[PATH_MAX];
} hash_arg_t;

static int
hash_job(job_t *job, void *p) {
  hash_arg_t *a = p;
  struct stat st;
  sha256_t sha;
  char hex[65];
  char *buf;
  int fd;

  if((fd = open(a->path, O_RDONLY)) < 0) {
    return job_fail(job, "open: %s", strerror(errno));
  }
  if(fstat(fd, &st) || !S_ISREG(st.st_mode)) {
    close(fd);
    return job_fail(job, "not a regular file");
  }
  if(!(buf = malloc(UPLOAD_BUF))) {
    close(fd);
    return job_fail(job, "out of memory");
  }
  job->bytes_total = (uint64_t)st.st_size;
  job->files_total = 1;
  sha256_init(&sha);
  for(;;) {
    ssize_t n = read(fd, buf, UPLOAD_BUF);
    if(n < 0 && errno == EINTR) {
      continue;
    }
    if(n < 0 || job->cancel) {
      free(buf);
      close(fd);
      return job_fail(job, n < 0 ? "read: %s" : "cancelled%s", n < 0 ? strerror(errno) : "");
    }
    if(!n) {
      break;
    }
    sha256_update(&sha, buf, (size_t)n);
    job->bytes_done += (uint64_t)n;
  }
  free(buf);
  close(fd);
  sha256_final(&sha, hex);
  job->files_done = 1;
  job_set_result(job, "%s", hex);
  return 0;
}

/* SHA-256 of a file on the console, as a job: the digest is the job result. */
void
api_fs_hash(http_req_t *req) {
  hash_arg_t *a = calloc(1, sizeof(hash_arg_t));
  char console[PATH_MAX], title[PATH_MAX + 16];
  jval_t *b;
  jw_t w;

  if(!a) {
    http_send_error(req, 500, "out of memory");
    return;
  }
  if(!(b = api_body(req))) {
    free(a);
    return;
  }
  if(api_body_path(req, b, "path", a->path, sizeof(a->path))) {
    json_free(b);
    free(a);
    return;
  }
  snprintf(console, sizeof(console), "%s", json_get_str(b, "path", ""));
  json_free(b);
  snprintf(title, sizeof(title), "SHA-256 %s", console);

  int id = job_start("hash", title, hash_job, a, free);
  if(id < 0) {
    http_send_error(req, 503, "too many jobs");
    return;
  }
  api_begin(&w);
  jw_kint(&w, "job", id);
  api_finish(req, &w);
}

/* ---- an upload on its way from the socket to the drive */

#define UP_SLOTS 8
#define UP_SLOT  (1024 * 1024)

typedef struct up_pipe {
  char           *buf[UP_SLOTS];
  size_t          len[UP_SLOTS];
  int             head, tail, count;
  int             done;  /* nothing more will come */
  int             error; /* errno of the write that failed */
  int             fd;
  uint64_t        written;
  pthread_mutex_t lock;
  pthread_cond_t  turn;
} up_pipe_t;

static void
up_free(up_pipe_t *up) {
  for(int i = 0; i < UP_SLOTS; i++) {
    free(up->buf[i]);
  }
  pthread_mutex_destroy(&up->lock);
  pthread_cond_destroy(&up->turn);
  free(up);
}

/* write what is waiting; with `wait`, keep at it until the upload is over */
static void
up_write(up_pipe_t *up, int wait) {
  pthread_mutex_lock(&up->lock);
  for(;;) {
    while(wait && !up->count && !up->done) {
      pthread_cond_wait(&up->turn, &up->lock);
    }
    if(!up->count) {
      break;
    }
    int slot = up->head;
    size_t left = up->len[slot];
    const char *p = up->buf[slot];
    int err = up->error;

    pthread_mutex_unlock(&up->lock);
    while(left && !err) {
      ssize_t n = write(up->fd, p, left);
      if(n < 0 && errno == EINTR) {
        continue;
      }
      if(n <= 0) {
        err = n < 0 ? errno : EIO;
        break;
      }
      p += n;
      left -= (size_t)n;
    }
    pthread_mutex_lock(&up->lock);
    up->written += up->len[slot] - left;
    up->error = err;
    up->head = (up->head + 1) % UP_SLOTS;
    up->count--;
    pthread_cond_broadcast(&up->turn);
  }
  pthread_mutex_unlock(&up->lock);
}

static void *
up_writer(void *arg) {
  up_write(arg, 1);
  return NULL;
}

void
api_fs_put(http_req_t *req) {
  char console[PATH_MAX], local[PATH_MAX];
  int64_t offset = http_query_int(req, "offset", -1);
  uint64_t written = 0;
  int fd;
  jw_t w;

  if(api_query_path(req, "path", console, sizeof(console), local, sizeof(local))) {
    return;
  }
  if(req->content_length < 0) {
    http_send_error(req, 411, "Content-Length required");
    return;
  }
  if(http_query_int(req, "mkdirs", 1)) {
    fs_mkparents(local);
  }

  /* size= is the size the file will have when the whole upload is done.
   * Refuse now what cannot fit, counting what is already there, and refuse
   * to resume past the end of what was really written. */
  int64_t total = http_query_int(req, "size", -1);
  struct stat st;
  int64_t have = !stat(local, &st) && S_ISREG(st.st_mode) ? (int64_t)st.st_size : 0;
  if(offset > have) {
    http_send_error(req, 409, "offset %lld is past the %lld bytes on the console",
                    (long long)offset, (long long)have);
    return;
  }
  if(total >= 0) {
    char dir[PATH_MAX];
    struct statvfs vfs;
    snprintf(dir, sizeof(dir), "%s", local);
    char *slash = strrchr(dir, '/');
    if(slash && slash != dir) {
      *slash = 0;
    }
    if(!statvfs(dir, &vfs)) {
      int64_t freeb = (int64_t)vfs.f_bavail * (int64_t)vfs.f_frsize;
      if(total - have > freeb) {
        http_send_error(req, 507, "not enough space: %lld bytes needed, %lld free",
                        (long long)(total - have), (long long)freeb);
        return;
      }
    }
  }

  int flags = O_WRONLY | O_CREAT;
  if(offset < 0) {
    flags |= O_TRUNC;
  }
  if((fd = open(local, flags, 0777)) < 0) {
    http_send_error(req, 500, "%s: %s", console, strerror(errno));
    return;
  }
  if(offset > 0 && lseek(fd, offset, SEEK_SET) < 0) {
    close(fd);
    http_send_error(req, 500, "seek: %s", strerror(errno));
    return;
  }
  /* The network and the drive each at their own pace: this thread takes the
   * data off the socket into a few buffers, another one writes them out. */
  up_pipe_t *up = calloc(1, sizeof(*up));
  pthread_t writer;
  int failed = 0, threaded = 0;

  if(up) {
    pthread_mutex_init(&up->lock, NULL);
    pthread_cond_init(&up->turn, NULL);
    up->fd = fd;
    for(int i = 0; i < UP_SLOTS; i++) {
      if(!(up->buf[i] = malloc(UP_SLOT))) {
        up_free(up);
        up = NULL;
        break;
      }
    }
  }
  if(!up) {
    close(fd);
    http_send_error(req, 500, "out of memory");
    return;
  }
  threaded = !pthread_create(&writer, NULL, up_writer, up);
  while(req->body_left > 0 && !failed) {
    size_t n = 0;
    int slot;

    pthread_mutex_lock(&up->lock);
    while(up->count == UP_SLOTS && !up->error) {
      pthread_cond_wait(&up->turn, &up->lock);
    }
    slot = up->tail;
    failed = up->error ? 2 : 0;
    pthread_mutex_unlock(&up->lock);
    if(failed) {
      break;
    }
    while(n < UP_SLOT && req->body_left > 0) {
      ssize_t got = http_read_body(req, up->buf[slot] + n, UP_SLOT - n);
      if(got <= 0) {
        failed = 1;
        break;
      }
      n += (size_t)got;
    }
    /* what did arrive is written even when the connection broke: the
     * upload continues from there */
    pthread_mutex_lock(&up->lock);
    up->len[slot] = n;
    up->tail = (up->tail + 1) % UP_SLOTS;
    up->count++;
    pthread_cond_broadcast(&up->turn);
    pthread_mutex_unlock(&up->lock);
    if(!threaded) {
      up_write(up, 0);
    }
  }
  pthread_mutex_lock(&up->lock);
  up->done = 1;
  pthread_cond_broadcast(&up->turn);
  pthread_mutex_unlock(&up->lock);
  if(threaded) {
    pthread_join(writer, NULL);
  } else {
    up_write(up, 0);
  }
  written = up->written;
  if(up->error) {
    errno = up->error;
    failed = 2;
  }
  up_free(up);
  if(close(fd) && !failed) {
    failed = 2;
  }

  if(failed == 1) {
    req->keep_alive = 0;
    LOG_WARN("upload of %s interrupted after %llu bytes", console,
             (unsigned long long)written);
    http_send_error(req, 400, "connection interrupted");
    return;
  }
  if(failed == 2) {
    req->keep_alive = 0;
    http_send_error(req, 500, "write %s: %s", console, strerror(errno));
    return;
  }

  api_begin(&w);
  jw_kstr(&w, "path", console);
  jw_kuint(&w, "written", written);
  api_finish(req, &w);
}
