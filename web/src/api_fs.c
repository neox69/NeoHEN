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
#include <unistd.h>

#include "api.h"
#include "fsutil.h"
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

void
api_fs_put(http_req_t *req) {
  char console[PATH_MAX], local[PATH_MAX];
  int64_t offset = http_query_int(req, "offset", -1);
  uint64_t written = 0;
  char *buf;
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
  if(!(buf = malloc(UPLOAD_BUF))) {
    close(fd);
    http_send_error(req, 500, "out of memory");
    return;
  }

  int failed = 0;
  while(req->body_left > 0) {
    ssize_t n = http_read_body(req, buf, UPLOAD_BUF);
    if(n <= 0) {
      failed = 1;
      break;
    }
    char *p = buf;
    while(n > 0) {
      ssize_t wn = write(fd, p, (size_t)n);
      if(wn <= 0) {
        if(wn < 0 && errno == EINTR) {
          continue;
        }
        failed = 2;
        break;
      }
      p += wn;
      n -= wn;
      written += (uint64_t)wn;
    }
    if(failed) {
      break;
    }
  }
  free(buf);
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
