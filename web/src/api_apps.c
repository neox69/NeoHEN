/* PS5AIO - library / application endpoints.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "api.h"
#include "fsutil.h"
#include "log.h"
#include "plat.h"
#include "sfo.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* where installed applications may live */
static const struct {
  const char *dir;
  const char *location;
} g_app_roots[] = {
  {"/user/app",           "internal"},
  {"/mnt/ext0/user/app",  "extended"},
  {"/mnt/ext1/user/app",  "m2"},
  {"/mnt/usb0/user/app",  "usb0"},
  {"/mnt/usb1/user/app",  "usb1"},
};

static int
exists(const char *console) {
  char local[PATH_MAX];
  struct stat st;
  plat_path(console, local, sizeof(local));
  return !stat(local, &st);
}

/* load a metadata file for a title, looking in appmeta first */
static int
load_meta_file(const char *tid, const char *name, char **data, size_t *len,
               char *found_console, size_t fsize) {
  char console[PATH_MAX], local[PATH_MAX];

  snprintf(console, sizeof(console), "/user/appmeta/%s/%s", tid, name);
  plat_path(console, local, sizeof(local));
  if(!fs_read_file(local, data, len, 4 * 1024 * 1024)) {
    if(found_console) {
      snprintf(found_console, fsize, "%s", console);
    }
    return 0;
  }
  for(size_t i = 0; i < sizeof(g_app_roots) / sizeof(g_app_roots[0]); i++) {
    snprintf(console, sizeof(console), "%s/%s/sce_sys/%s", g_app_roots[i].dir, tid, name);
    plat_path(console, local, sizeof(local));
    if(!fs_read_file(local, data, len, 4 * 1024 * 1024)) {
      if(found_console) {
        snprintf(found_console, fsize, "%s", console);
      }
      return 0;
    }
  }
  return -1;
}

/* best title name from a param.json tree */
static const char *
pj_title(const jval_t *pj) {
  jval_t *lp = json_get(pj, "localizedParameters");
  const char *def = json_get_str(lp, "defaultLanguage", "en-US");
  const char *t = json_get_str(json_get(lp, def), "titleName", NULL);
  if(!t) {
    t = json_get_str(json_get(lp, "en-US"), "titleName", NULL);
  }
  if(!t && lp) {
    for(jval_t *c = lp->child; c && !t; c = c->next) {
      if(c->type == J_OBJ) {
        t = json_get_str(c, "titleName", NULL);
      }
    }
  }
  return t;
}

static void
write_app(jw_t *w, const char *tid) {
  char *data = NULL;
  size_t len = 0;
  char src[PATH_MAX] = "";

  jw_obj(w);
  jw_kstr(w, "titleId", tid);

  if(!load_meta_file(tid, "param.json", &data, &len, src, sizeof(src))) {
    jval_t *pj = json_parse(data, len);
    jw_kstr(w, "format", "ps5");
    jw_kstr(w, "paramPath", src);
    if(pj) {
      jw_kstr(w, "title", pj_title(pj) ? pj_title(pj) : tid);
      jw_kstr(w, "contentId", json_get_str(pj, "contentId", ""));
      jw_kstr(w, "version", json_get_str(pj, "contentVersion",
                                         json_get_str(pj, "masterVersion", "")));
      jw_kstr(w, "category", json_get_str(pj, "applicationCategoryType", ""));
      {
        jval_t *lp = json_get(pj, "localizedParameters");
        jw_kstr(w, "defaultLanguage", json_get_str(lp, "defaultLanguage", ""));
      }
      json_free(pj);
    } else {
      jw_kstr(w, "title", tid);
    }
    free(data);
  } else if(!load_meta_file(tid, "param.sfo", &data, &len, src, sizeof(src))) {
    char buf[512];
    uint32_t attr;
    jw_kstr(w, "format", "ps4");
    jw_kstr(w, "paramPath", src);
    jw_kstr(w, "title", sfo_get_str((uint8_t *)data, len, "TITLE", buf, sizeof(buf)) ? tid : buf);
    if(!sfo_get_str((uint8_t *)data, len, "CONTENT_ID", buf, sizeof(buf))) {
      jw_kstr(w, "contentId", buf);
    }
    if(!sfo_get_str((uint8_t *)data, len, "APP_VER", buf, sizeof(buf))) {
      jw_kstr(w, "version", buf);
    }
    if(!sfo_get_str((uint8_t *)data, len, "CATEGORY", buf, sizeof(buf))) {
      jw_kstr(w, "category", buf);
    }
    if(!sfo_get_int((uint8_t *)data, len, "ATTRIBUTE", &attr)) {
      jw_kuint(w, "attribute", attr);
    }
    free(data);
  } else {
    jw_kstr(w, "format", "unknown");
    jw_kstr(w, "title", tid);
  }

  {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "/user/appmeta/%s/icon0.png", tid);
    jw_kbool(w, "hasIcon", exists(p));
    snprintf(p, sizeof(p), "/user/appmeta/%s/pic0.png", tid);
    jw_kbool(w, "hasPic0", exists(p));
    snprintf(p, sizeof(p), "/user/appmeta/%s/pic1.png", tid);
    jw_kbool(w, "hasPic1", exists(p));
  }

  jw_key(w, "locations");
  jw_arr(w);
  for(size_t i = 0; i < sizeof(g_app_roots) / sizeof(g_app_roots[0]); i++) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", g_app_roots[i].dir, tid);
    if(exists(p)) {
      jw_obj(w);
      jw_kstr(w, "location", g_app_roots[i].location);
      jw_kstr(w, "path", p);
      jw_obj_end(w);
    }
  }
  jw_arr_end(w);
  jw_obj_end(w);
}

typedef struct tid_list {
  char (*ids)[16];
  size_t n, cap;
} tid_list_t;

static void
tid_add(tid_list_t *l, const char *tid) {
  for(size_t i = 0; i < l->n; i++) {
    if(!strcmp(l->ids[i], tid)) {
      return;
    }
  }
  if(l->n == l->cap) {
    size_t cap = l->cap ? l->cap * 2 : 128;
    void *p = realloc(l->ids, cap * sizeof(*l->ids));
    if(!p) {
      return;
    }
    l->ids = p;
    l->cap = cap;
  }
  snprintf(l->ids[l->n++], 16, "%s", tid);
}

static void
scan_dir(tid_list_t *l, const char *console_dir) {
  char local[PATH_MAX];
  DIR *d;
  struct dirent *e;
  plat_path(console_dir, local, sizeof(local));
  if(!(d = opendir(local))) {
    return;
  }
  while((e = readdir(d))) {
    if(is_title_id(e->d_name)) {
      tid_add(l, e->d_name);
    }
  }
  closedir(d);
}

static int
tid_cmp(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

void
api_apps_list(http_req_t *req) {
  tid_list_t l = {0};
  char filter[64] = "";
  jw_t w;

  http_query(req, "id", filter, sizeof(filter));

  scan_dir(&l, "/user/appmeta");
  for(size_t i = 0; i < sizeof(g_app_roots) / sizeof(g_app_roots[0]); i++) {
    scan_dir(&l, g_app_roots[i].dir);
  }
  if(l.n) {
    qsort(l.ids, l.n, sizeof(*l.ids), tid_cmp);
  }

  api_begin(&w);
  jw_key(&w, "apps");
  jw_arr(&w);
  for(size_t i = 0; i < l.n; i++) {
    if(filter[0] && strcmp(filter, l.ids[i])) {
      continue;
    }
    write_app(&w, l.ids[i]);
  }
  jw_arr_end(&w);
  free(l.ids);
  api_finish(req, &w);
}

void
api_apps_param(http_req_t *req) {
  char tid[32], src[PATH_MAX];
  char *data = NULL;
  size_t len = 0;
  jw_t w;

  http_query(req, "id", tid, sizeof(tid));
  if(!is_title_id(tid)) {
    http_send_error(req, 400, "invalid title id");
    return;
  }

  api_begin(&w);
  jw_kstr(&w, "titleId", tid);
  if(!load_meta_file(tid, "param.json", &data, &len, src, sizeof(src))) {
    jval_t *pj = json_parse(data, len);
    jw_kstr(&w, "format", "json");
    jw_kstr(&w, "path", src);
    jw_key(&w, "param");
    if(pj) {
      json_write(&w, pj);
      json_free(pj);
    } else {
      jw_null(&w);
    }
    jw_key(&w, "raw");
    jw_strn(&w, data, len);
    free(data);
  } else if(!load_meta_file(tid, "param.sfo", &data, &len, src, sizeof(src))) {
    jw_kstr(&w, "format", "sfo");
    jw_kstr(&w, "path", src);
    jw_key(&w, "param");
    jw_obj(&w);
    sfo_to_json(&w, (uint8_t *)data, len);
    jw_obj_end(&w);
    free(data);
  } else {
    jw_free(&w);
    http_send_error(req, 404, "no param.json/param.sfo for %s", tid);
    return;
  }
  api_finish(req, &w);
}

void
api_apps_icon(http_req_t *req) {
  static const char *kinds[] = {"icon0", "icon1", "pic0", "pic1", "pic2", "save_data"};
  char tid[32], kind[32], console[PATH_MAX], local[PATH_MAX];
  struct stat st;
  int ok = 0;

  http_query(req, "id", tid, sizeof(tid));
  if(!http_query(req, "kind", kind, sizeof(kind)) || !kind[0]) {
    snprintf(kind, sizeof(kind), "icon0");
  }
  for(size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
    if(!strcmp(kind, kinds[i])) {
      ok = 1;
    }
  }
  if(!is_title_id(tid) || !ok) {
    http_send_error(req, 400, "invalid id or kind");
    return;
  }

  snprintf(console, sizeof(console), "/user/appmeta/%s/%s.png", tid, kind);
  plat_path(console, local, sizeof(local));
  if(stat(local, &st)) {
    for(size_t i = 0; i < sizeof(g_app_roots) / sizeof(g_app_roots[0]); i++) {
      snprintf(console, sizeof(console), "%s/%s/sce_sys/%s.png", g_app_roots[i].dir, tid, kind);
      plat_path(console, local, sizeof(local));
      if(!stat(local, &st)) {
        break;
      }
    }
  }
  http_send_file(req, local, 0);
}

/* rewrite a title's name in every param file we can find */
static int
rename_param_json(const char *console, const char *name, const char *lang) {
  char local[PATH_MAX];
  char *data = NULL;
  size_t len = 0;
  int r = -1;

  plat_path(console, local, sizeof(local));
  if(fs_read_file(local, &data, &len, 4 * 1024 * 1024)) {
    return -1;
  }
  jval_t *pj = json_parse(data, len);
  free(data);
  if(!pj) {
    return -1;
  }
  jval_t *lp = json_get_or_add_obj(pj, "localizedParameters");
  if(lp) {
    int changed = 0;
    if(lang && *lang) {
      jval_t *l = json_get_or_add_obj(lp, lang);
      changed = l && json_set_str(l, "titleName", name);
    } else {
      for(jval_t *c = lp->child; c; c = c->next) {
        if(c->type == J_OBJ && json_set_str(c, "titleName", name)) {
          changed = 1;
        }
      }
      if(!changed) {
        jval_t *l = json_get_or_add_obj(lp, json_get_str(lp, "defaultLanguage", "en-US"));
        changed = l && json_set_str(l, "titleName", name);
      }
    }
    if(changed) {
      jw_t w;
      jw_init(&w);
      json_write(&w, pj);
      if(!w.oom) {
        r = fs_write_file(local, w.data, w.len);
      }
      jw_free(&w);
    }
  }
  json_free(pj);
  return r;
}

static int
rename_param_sfo(const char *console, const char *name) {
  char local[PATH_MAX];
  char *data = NULL;
  size_t len = 0;
  int changed = 0;

  plat_path(console, local, sizeof(local));
  if(fs_read_file(local, &data, &len, 1024 * 1024)) {
    return -1;
  }
  if(!sfo_set_str((uint8_t *)data, len, "TITLE", name)) {
    changed = 1;
  }
  for(int i = 0; i < 30; i++) {
    char key[16];
    snprintf(key, sizeof(key), "TITLE_%02d", i);
    if(!sfo_set_str((uint8_t *)data, len, key, name)) {
      changed = 1;
    }
  }
  int r = changed ? fs_write_file(local, data, len) : -1;
  free(data);
  return r;
}

void
api_apps_rename(http_req_t *req) {
  jval_t *b = api_body(req);
  char console[PATH_MAX];
  int updated = 0;
  jw_t w;

  if(!b) {
    return;
  }
  const char *tid = json_get_str(b, "titleId", "");
  const char *name = json_get_str(b, "name", "");
  const char *lang = json_get_str(b, "lang", NULL);
  if(!is_title_id(tid) || !*name || strlen(name) > 127) {
    json_free(b);
    http_send_error(req, 400, "invalid titleId or name");
    return;
  }

  api_begin(&w);
  jw_key(&w, "updated");
  jw_arr(&w);

  snprintf(console, sizeof(console), "/user/appmeta/%s/param.json", tid);
  if(!rename_param_json(console, name, lang)) {
    jw_str(&w, console);
    updated++;
  }
  snprintf(console, sizeof(console), "/user/appmeta/%s/param.sfo", tid);
  if(!rename_param_sfo(console, name)) {
    jw_str(&w, console);
    updated++;
  }
  for(size_t i = 0; i < sizeof(g_app_roots) / sizeof(g_app_roots[0]); i++) {
    snprintf(console, sizeof(console), "%s/%s/sce_sys/param.json", g_app_roots[i].dir, tid);
    if(!rename_param_json(console, name, lang)) {
      jw_str(&w, console);
      updated++;
    }
    snprintf(console, sizeof(console), "%s/%s/sce_sys/param.sfo", g_app_roots[i].dir, tid);
    if(!rename_param_sfo(console, name)) {
      jw_str(&w, console);
      updated++;
    }
  }
  jw_arr_end(&w);
  jw_kstr(&w, "note", "the home screen caches names in app.db; update it too and restart the shell");

  if(!updated) {
    jw_free(&w);
    http_send_error(req, 404, "no writable param file found for %s", tid);
    json_free(b);
    return;
  }
  LOG_INFO("renamed %s to '%s' (%d files)", tid, name, updated);
  json_free(b);
  api_finish(req, &w);
}

void
api_apps_launch(http_req_t *req) {
  jval_t *b = api_body(req);
  if(!b) {
    return;
  }
  const char *tid = json_get_str(b, "titleId", "");
  if(!is_title_id(tid)) {
    json_free(b);
    http_send_error(req, 400, "invalid title id");
    return;
  }
  int r = plat_app_launch(tid, json_get_str(b, "args", ""));
  json_free(b);
  if(r) {
    http_send_error(req, 500, "%s", plat_error());
    return;
  }
  http_send_ok(req);
}

void
api_apps_kill(http_req_t *req) {
  http_drain_body(req);
  if(plat_app_kill_running()) {
    http_send_error(req, 409, "%s", plat_error());
    return;
  }
  http_send_ok(req);
}

void
api_apps_running(http_req_t *req) {
  char tid[16] = "";
  int app_id = 0;
  pid_t pid = -1;
  jw_t w;

  api_begin(&w);
  if(plat_app_running(tid, sizeof(tid), &app_id, &pid)) {
    jw_kbool(&w, "running", 0);
  } else {
    jw_kbool(&w, "running", 1);
    jw_kstr(&w, "titleId", tid);
    jw_khex(&w, "appId", (uint32_t)app_id);
    jw_kint(&w, "pid", pid);
  }
  api_finish(req, &w);
}

void
api_apps_install_dir(http_req_t *req) {
  jval_t *b = api_body(req);
  if(!b) {
    return;
  }
  const char *tid = json_get_str(b, "titleId", "");
  const char *dir = json_get_str(b, "dir", "/user/app/");
  if(!is_title_id(tid)) {
    json_free(b);
    http_send_error(req, 400, "invalid title id");
    return;
  }
  int r = plat_app_install_dir(tid, dir);
  json_free(b);
  if(r) {
    http_send_error(req, 500, "%s", plat_error());
    return;
  }
  http_send_ok(req);
}

void
api_apps_install_pkg(http_req_t *req) {
  char cid[64] = "";
  jw_t w;
  jval_t *b = api_body(req);
  if(!b) {
    return;
  }
  const char *url = json_get_str(b, "url", "");
  if(!*url) {
    json_free(b);
    http_send_error(req, 400, "missing url");
    return;
  }
  int r = plat_app_install_pkg(url, json_get_str(b, "name", ""),
                               json_get_str(b, "iconUrl", ""), cid, sizeof(cid));
  json_free(b);
  if(r) {
    http_send_error(req, 500, "%s", plat_error());
    return;
  }
  api_begin(&w);
  jw_kstr(&w, "contentId", cid);
  api_finish(req, &w);
}

void
api_apps_uninstall(http_req_t *req) {
  jval_t *b = api_body(req);
  if(!b) {
    return;
  }
  const char *tid = json_get_str(b, "titleId", "");
  if(!is_title_id(tid)) {
    json_free(b);
    http_send_error(req, 400, "invalid title id");
    return;
  }
  int r = plat_app_uninstall(tid);
  json_free(b);
  if(r) {
    http_send_error(req, 500, "%s", plat_error());
    return;
  }
  http_send_ok(req);
}
