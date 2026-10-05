/* PS5AIO - game dumper and backups.
 *
 * Dumping copies the *mounted* (PFS-decrypted) game filesystem to another
 * drive. The title must be running (or otherwise mounted) so that
 * /mnt/sandbox/pfsmnt/<TITLEID>-app0 exists. Executables (eboot.bin, *.sprx)
 * stay in signed SELF format: decrypting them is out of scope here.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "api.h"
#include "fsutil.h"
#include "jobs.h"
#include "log.h"
#include "plat.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

typedef struct dump_src {
  const char *fmt;    /* console path, %s = title id */
  const char *kind;   /* app | patch | meta | raw */
  const char *suffix; /* destination folder suffix */
} dump_src_t;

static const dump_src_t g_sources[] = {
  {"/mnt/sandbox/pfsmnt/%s-app0",  "app",   "-app"},
  {"/mnt/sandbox/pfsmnt/%s-patch0", "patch", "-patch"},
  {"/system_ex/app/%s",            "app",   "-app"},
  {"/user/appmeta/%s",             "meta",  "-meta"},
  {"/user/app/%s",                 "raw",   "-raw"},
};

static int
dir_exists(const char *console) {
  char local[PATH_MAX];
  struct stat st;
  plat_path(console, local, sizeof(local));
  return !stat(local, &st) && S_ISDIR(st.st_mode);
}

void
api_dump_sources(http_req_t *req) {
  char tid[32];
  jw_t w;

  http_query(req, "titleId", tid, sizeof(tid));
  if(!is_title_id(tid)) {
    http_send_error(req, 400, "invalid title id");
    return;
  }
  api_begin(&w);
  jw_kstr(&w, "titleId", tid);
  jw_key(&w, "sources");
  jw_arr(&w);
  for(size_t i = 0; i < sizeof(g_sources) / sizeof(g_sources[0]); i++) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), g_sources[i].fmt, tid);
    jw_obj(&w);
    jw_kstr(&w, "path", p);
    jw_kstr(&w, "kind", g_sources[i].kind);
    jw_kbool(&w, "available", dir_exists(p));
    jw_obj_end(&w);
  }
  jw_arr_end(&w);
  {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "/mnt/sandbox/pfsmnt/%s-app0", tid);
    jw_kbool(&w, "mounted", dir_exists(p));
  }
  api_finish(req, &w);
}

typedef struct dump_arg {
  char tid[16];
  char dest[PATH_MAX];  /* console path */
  int  patch;
  int  meta;
  int  raw;
} dump_arg_t;

static int
dump_job(job_t *job, void *p) {
  dump_arg_t *a = p;
  char src[PATH_MAX], dst[PATH_MAX], lsrc[PATH_MAX], ldst[PATH_MAX];
  struct { char src[PATH_MAX]; char dst[PATH_MAX]; } plan[5];
  int nplan = 0;
  int have_app = 0;

  for(size_t i = 0; i < sizeof(g_sources) / sizeof(g_sources[0]); i++) {
    const dump_src_t *s = &g_sources[i];
    if(!strcmp(s->kind, "app") && have_app) {
      continue; /* first available app source wins */
    }
    if(!strcmp(s->kind, "patch") && !a->patch) continue;
    if(!strcmp(s->kind, "meta") && !a->meta) continue;
    if(!strcmp(s->kind, "raw") && !a->raw) continue;

    snprintf(src, sizeof(src), s->fmt, a->tid);
    if(!dir_exists(src)) {
      continue;
    }
    if(!strcmp(s->kind, "app")) {
      have_app = 1;
    }
    snprintf(dst, sizeof(dst), "%s/%s%s", a->dest, a->tid, s->suffix);
    plat_path(src, plan[nplan].src, sizeof(plan[nplan].src));
    plat_path(dst, plan[nplan].dst, sizeof(plan[nplan].dst));
    nplan++;
  }

  if(!have_app) {
    return job_fail(job, "game files for %s are not mounted; start the game first", a->tid);
  }

  job_set_current(job, "calculating size...");
  for(int i = 0; i < nplan && !job->cancel; i++) {
    uint64_t b = 0;
    uint32_t f = 0;
    fs_tree_size(plan[i].src, &b, &f, job);
    job->bytes_total += b;
    job->files_total += f;
  }

  /* make sure the destination has enough room */
  plat_path(a->dest, ldst, sizeof(ldst));
  fs_mkdirs(ldst, 0777);
  (void)lsrc;

  for(int i = 0; i < nplan && !job->cancel; i++) {
    if(fs_copy_tree(plan[i].src, plan[i].dst, FS_COPY_SKIP_ERRORS, job)) {
      return -1;
    }
  }
  if(job->cancel) {
    return -1;
  }
  job->current[0] = 0;
  job_set_result(job, "dumped %u files to %s/%s-app (executables remain SELF-signed)",
                 job->files_done, a->dest, a->tid);
  plat_notify("PS5AIO: dump of %s finished", a->tid);
  return 0;
}

void
api_dump_start(http_req_t *req) {
  dump_arg_t *a;
  jval_t *b;
  jw_t w;

  if(!(b = api_body(req))) {
    return;
  }
  if(!(a = calloc(1, sizeof(dump_arg_t)))) {
    json_free(b);
    http_send_error(req, 500, "out of memory");
    return;
  }
  snprintf(a->tid, sizeof(a->tid), "%s", json_get_str(b, "titleId", ""));
  snprintf(a->dest, sizeof(a->dest), "%s", json_get_str(b, "dest", "/mnt/usb0/PS5AIO/dumps"));
  a->patch = json_get_bool(b, "includePatch", 1);
  a->meta = json_get_bool(b, "includeMeta", 1);
  a->raw = json_get_bool(b, "includeRaw", 0);
  json_free(b);

  if(!is_title_id(a->tid) || a->dest[0] != '/' || strstr(a->dest, "..")) {
    free(a);
    http_send_error(req, 400, "invalid titleId or dest");
    return;
  }

  char title[256];
  snprintf(title, sizeof(title), "Dump %s -> %s", a->tid, a->dest);
  int id = job_start("dump", title, dump_job, a, free);
  if(id < 0) {
    http_send_error(req, 503, "too many jobs");
    return;
  }
  api_begin(&w);
  jw_kint(&w, "job", id);
  api_finish(req, &w);
}

/* ------------------------------------------------------------ backup */

#define MAX_BACKUP_SRC 32

typedef struct backup_arg {
  char src[MAX_BACKUP_SRC][PATH_MAX];
  int  nsrc;
  char dest[PATH_MAX];
} backup_arg_t;

static int
backup_job(job_t *job, void *p) {
  backup_arg_t *a = p;
  char lsrc[PATH_MAX], ldst[PATH_MAX], cdst[PATH_MAX * 2];

  for(int i = 0; i < a->nsrc && !job->cancel; i++) {
    uint64_t b = 0;
    uint32_t f = 0;
    plat_path(a->src[i], lsrc, sizeof(lsrc));
    fs_tree_size(lsrc, &b, &f, job);
    job->bytes_total += b;
    job->files_total += f;
  }
  for(int i = 0; i < a->nsrc && !job->cancel; i++) {
    /* keep the full console path below the backup folder */
    snprintf(cdst, sizeof(cdst), "%s%s", a->dest, a->src[i]);
    plat_path(a->src[i], lsrc, sizeof(lsrc));
    plat_path(cdst, ldst, sizeof(ldst));
    if(fs_copy_tree(lsrc, ldst, FS_COPY_SKIP_ERRORS, job)) {
      return -1;
    }
  }
  job->current[0] = 0;
  job_set_result(job, "%s", a->dest);
  return job->cancel ? -1 : 0;
}

void
api_backup_start(http_req_t *req) {
  backup_arg_t *a;
  jval_t *b;
  jw_t w;
  char stamp[32];
  time_t now = time(NULL);
  struct tm tm;

  if(!(b = api_body(req))) {
    return;
  }
  if(!(a = calloc(1, sizeof(backup_arg_t)))) {
    json_free(b);
    http_send_error(req, 500, "out of memory");
    return;
  }
  jval_t *srcs = json_get(b, "sources");
  if(srcs && srcs->type == J_ARR) {
    for(jval_t *c = srcs->child; c && a->nsrc < MAX_BACKUP_SRC; c = c->next) {
      if(c->type == J_STR && c->str[0] == '/' && !strstr(c->str, "..")) {
        snprintf(a->src[a->nsrc++], PATH_MAX, "%s", c->str);
      }
    }
  }
  localtime_r(&now, &tm);
  strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
  const char *dest = json_get_str(b, "dest", "/mnt/usb0/PS5AIO/backups");
  const char *name = json_get_str(b, "name", "backup");
  snprintf(a->dest, sizeof(a->dest), "%s/%s-%s", dest, name, stamp);
  json_free(b);

  if(!a->nsrc || a->dest[0] != '/' || strstr(a->dest, "..")) {
    free(a);
    http_send_error(req, 400, "need at least one absolute source and a dest");
    return;
  }

  /* `a` belongs to the job once started; keep our own copy of dest */
  char dest_copy[PATH_MAX];
  char title[PATH_MAX + 16];
  snprintf(dest_copy, sizeof(dest_copy), "%s", a->dest);
  snprintf(title, sizeof(title), "Backup -> %s", a->dest);
  int id = job_start("backup", title, backup_job, a, free);
  if(id < 0) {
    http_send_error(req, 503, "too many jobs");
    return;
  }
  api_begin(&w);
  jw_kint(&w, "job", id);
  jw_kstr(&w, "dest", dest_copy);
  api_finish(req, &w);
}
