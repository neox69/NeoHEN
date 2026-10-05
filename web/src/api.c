/* PS5AIO - REST API: router, shared helpers and system endpoints.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "fsutil.h"
#include "jobs.h"
#include "log.h"
#include "plat.h"

static char g_token[128]; /* optional shared secret */

/* ------------------------------------------------------------ helpers */

jval_t *
api_body(http_req_t *req) {
  jval_t *v = http_read_json(req);
  if(!v || v->type != J_OBJ) {
    json_free(v);
    http_send_error(req, 400, "invalid JSON body");
    return NULL;
  }
  return v;
}

int
api_map_path(http_req_t *req, const char *console, char *local, size_t size) {
  if(!console || console[0] != '/') {
    http_send_error(req, 400, "path must be absolute");
    return -1;
  }
  /* refuse ".." components: keeps the host emulator inside its root and
   * avoids surprises on the console */
  for(const char *p = console; (p = strstr(p, "..")); p += 2) {
    if((p == console || p[-1] == '/') && (p[2] == 0 || p[2] == '/')) {
      http_send_error(req, 400, "'..' is not allowed in paths");
      return -1;
    }
  }
  plat_path(console, local, size);
  return 0;
}

int
api_query_path(http_req_t *req, const char *key, char *console, size_t csize,
               char *local, size_t lsize) {
  if(!http_query(req, key, console, csize) || !console[0]) {
    http_send_error(req, 400, "missing '%s' parameter", key);
    return -1;
  }
  return api_map_path(req, console, local, lsize);
}

int
api_body_path(http_req_t *req, const jval_t *body, const char *key, char *local,
              size_t lsize) {
  const char *p = json_get_str(body, key, NULL);
  if(!p || !*p) {
    http_send_error(req, 400, "missing '%s'", key);
    return -1;
  }
  return api_map_path(req, p, local, lsize);
}

void
api_begin(jw_t *w) {
  jw_init(w);
  jw_obj(w);
  jw_kbool(w, "ok", 1);
}

void
api_finish(http_req_t *req, jw_t *w) {
  jw_obj_end(w);
  http_send_json(req, 200, w);
  jw_free(w);
}

int
is_title_id(const char *s) {
  /* e.g. PPSA01234, CUSA01234, NPXS40000 */
  if(!s || strlen(s) != 9) {
    return 0;
  }
  for(int i = 0; i < 4; i++) {
    if(!isupper((unsigned char)s[i])) {
      return 0;
    }
  }
  for(int i = 4; i < 9; i++) {
    if(!isdigit((unsigned char)s[i])) {
      return 0;
    }
  }
  return 1;
}

/* ------------------------------------------------------------ system */

static void
api_ping(http_req_t *req) {
  jw_t w;
  api_begin(&w);
  jw_kstr(&w, "name", PS5AIO_NAME);
  jw_kstr(&w, "version", PS5AIO_VERSION);
  jw_kstr(&w, "platform", plat_name());
  jw_kbool(&w, "auth", g_token[0] != 0);
  api_finish(req, &w);
}

static void
api_sys_info(http_req_t *req) {
  jw_t w;
  api_begin(&w);
  jw_kstr(&w, "version", PS5AIO_VERSION);
  plat_sysinfo(&w);
  {
    char tid[16] = "";
    int app_id = 0;
    pid_t pid = -1;
    if(!plat_app_running(tid, sizeof(tid), &app_id, &pid)) {
      jw_key(&w, "running");
      jw_obj(&w);
      jw_kstr(&w, "titleId", tid);
      jw_khex(&w, "appId", (uint32_t)app_id);
      jw_kint(&w, "pid", pid);
      jw_obj_end(&w);
    }
  }
  api_finish(req, &w);
}

static void
api_sys_sensors(http_req_t *req) {
  jw_t w;
  api_begin(&w);
  plat_sensors(&w);
  api_finish(req, &w);
}

static void
api_sys_mounts(http_req_t *req) {
  jw_t w;
  api_begin(&w);
  jw_key(&w, "mounts");
  plat_mounts(&w);
  api_finish(req, &w);
}

static void
api_sys_procs(http_req_t *req) {
  jw_t w;
  api_begin(&w);
  jw_key(&w, "procs");
  plat_procs(&w);
  api_finish(req, &w);
}

static void
api_sys_kill(http_req_t *req) {
  jval_t *b = api_body(req);
  if(!b) {
    return;
  }
  int pid = (int)json_get_int(b, "pid", 0);
  int sig = (int)json_get_int(b, "signal", SIGKILL);
  json_free(b);
  if(pid <= 1) {
    http_send_error(req, 400, "invalid pid");
    return;
  }
  if(kill(pid, sig)) {
    http_send_error(req, 500, "kill(%d, %d) failed", pid, sig);
    return;
  }
  http_send_ok(req);
}

static void
api_sys_notify(http_req_t *req) {
  jval_t *b = api_body(req);
  if(!b) {
    return;
  }
  const char *text = json_get_str(b, "text", "");
  plat_notify("%s", text);
  json_free(b);
  http_send_ok(req);
}

static void
api_sys_power(http_req_t *req) {
  jval_t *b = api_body(req);
  char action[32];
  if(!b) {
    return;
  }
  snprintf(action, sizeof(action), "%s", json_get_str(b, "action", ""));
  json_free(b);
  /* reply first: the console goes away right after */
  http_send_ok(req);
  if(plat_power(action)) {
    LOG_ERR("power %s: %s", action, plat_error());
  }
}

static void
api_sys_browser(http_req_t *req) {
  jval_t *b = api_body(req);
  if(!b) {
    return;
  }
  int r = plat_open_browser(json_get_str(b, "url", "about:blank"));
  json_free(b);
  if(r) {
    http_send_error(req, 500, "%s", plat_error());
    return;
  }
  http_send_ok(req);
}

static void
api_sys_log(http_req_t *req) {
  jw_t w;
  api_begin(&w);
  jw_key(&w, "entries");
  log_dump_json(&w, (unsigned)http_query_int(req, "since", 0));
  api_finish(req, &w);
}

/* ------------------------------------------------------------ jobs */

static void
api_jobs_list(http_req_t *req) {
  jw_t w;
  int id = (int)http_query_int(req, "id", 0);
  api_begin(&w);
  if(id > 0) {
    jw_key(&w, "job");
    if(job_json(&w, id)) {
      jw_null(&w);
    }
  } else {
    jw_key(&w, "jobs");
    jobs_json(&w);
  }
  api_finish(req, &w);
}

static void
api_jobs_cancel(http_req_t *req) {
  jval_t *b = api_body(req);
  if(!b) {
    return;
  }
  int id = (int)json_get_int(b, "id", 0);
  json_free(b);
  if(job_cancel(id)) {
    http_send_error(req, 404, "no such job");
    return;
  }
  http_send_ok(req);
}

static void
api_jobs_clear(http_req_t *req) {
  http_drain_body(req);
  jobs_clear();
  http_send_ok(req);
}

/* ------------------------------------------------------------ routing */

/*
 * The default build is the "safe" set: it reads the console's state, can
 * launch or close a game, and manages the payloads in NeoHEN's own payloads
 * folder (upload, start, stop, delete, auto-start), by bare file name only.
 * The file manager, installer, memory tools, dumper and power routes are
 * only compiled into a NEOWEB_FULL build (make FULL=1).
 */
static const api_route_t g_routes[] = {
  {"GET",  "/api/ping",             api_ping},
  {"GET",  "/api/sys/info",         api_sys_info},
  {"GET",  "/api/sys/sensors",      api_sys_sensors},
  {"GET",  "/api/sys/mounts",       api_sys_mounts},
  {"GET",  "/api/sys/procs",        api_sys_procs},
  {"POST", "/api/sys/notify",       api_sys_notify},
  {"GET",  "/api/sys/log",          api_sys_log},

  {"GET",  "/api/apps/list",        api_apps_list},
  {"GET",  "/api/apps/param",       api_apps_param},
  {NULL,   "/apps/icon",            api_apps_icon},
  {"POST", "/api/apps/launch",      api_apps_launch},
  {"POST", "/api/apps/kill",        api_apps_kill},
  {"GET",  "/api/apps/running",     api_apps_running},

  {"GET",  "/api/payloads/list",      api_payloads_list},
  {"PUT",  "/api/payloads/upload",    api_payloads_upload},
  {"POST", "/api/payloads/upload",    api_payloads_upload},
  {"POST", "/api/payloads/start",     api_payloads_start},
  {"POST", "/api/payloads/stop",      api_payloads_stop},
  {"POST", "/api/payloads/delete",    api_payloads_delete},
  {"POST", "/api/payloads/autostart", api_payloads_autostart},

#ifdef NEOWEB_FULL
  {"POST", "/api/sys/kill",         api_sys_kill},
  {"POST", "/api/sys/power",        api_sys_power},
  {"POST", "/api/sys/browser",      api_sys_browser},

  {"GET",  "/api/jobs",             api_jobs_list},
  {"POST", "/api/jobs/cancel",      api_jobs_cancel},
  {"POST", "/api/jobs/clear",       api_jobs_clear},

  {"GET",  "/api/fs/list",          api_fs_list},
  {"GET",  "/api/fs/stat",          api_fs_stat},
  {"GET",  "/api/fs/walk",          api_fs_walk},
  {"GET",  "/api/fs/size",          api_fs_size},
  {"GET",  "/api/fs/text",          api_fs_read_text},
  {"POST", "/api/fs/text",          api_fs_write_text},
  {"POST", "/api/fs/mkdir",         api_fs_mkdir},
  {"POST", "/api/fs/delete",        api_fs_delete},
  {"POST", "/api/fs/rename",        api_fs_rename},
  {"POST", "/api/fs/copy",          api_fs_copy},
  {"POST", "/api/fs/chmod",         api_fs_chmod},
  {NULL,   "/fs/get",               api_fs_get},
  {"PUT",  "/fs/put",               api_fs_put},
  {"POST", "/fs/put",               api_fs_put},

  {"POST", "/api/apps/rename",      api_apps_rename},
  {"POST", "/api/apps/install-dir", api_apps_install_dir},
  {"POST", "/api/apps/install-pkg", api_apps_install_pkg},
  {"POST", "/api/apps/uninstall",   api_apps_uninstall},

  {"GET",  "/api/mem/maps",         api_mem_maps},
  {"GET",  "/api/mem/read",         api_mem_read},
  {"POST", "/api/mem/write",        api_mem_write},
  {"POST", "/api/mem/scan",         api_mem_scan},
  {"GET",  "/api/mem/results",      api_mem_results},
  {"POST", "/api/mem/reset",        api_mem_reset},
  {"POST", "/api/mem/freeze",       api_mem_freeze},
  {"GET",  "/api/mem/freeze",       api_mem_freeze_list},
  {"POST", "/api/mem/unfreeze",     api_mem_unfreeze},

  {"GET",  "/api/dump/sources",     api_dump_sources},
  {"POST", "/api/dump/start",       api_dump_start},
  {"POST", "/api/backup/start",     api_backup_start},
#endif
};

static void
load_config(void) {
  char path[1024];
  char *data = NULL;
  size_t len = 0;

  plat_path(PS5AIO_DATA_DIR "/config.json", path, sizeof(path));
  if(fs_read_file(path, &data, &len, 64 * 1024)) {
    return;
  }
  jval_t *cfg = json_parse(data, len);
  free(data);
  if(!cfg) {
    LOG_WARN("config.json is not valid JSON");
    return;
  }
  snprintf(g_token, sizeof(g_token), "%s", json_get_str(cfg, "token", ""));
  json_free(cfg);
  if(g_token[0]) {
    LOG_INFO("API token authentication enabled");
  }
}

void
api_init(void) {
  char path[1024];
  plat_path(PS5AIO_DATA_DIR, path, sizeof(path));
  fs_mkdirs(path, 0777);
  load_config();
#ifdef NEOWEB_FULL
  api_mem_init();
#endif
}

static int
authorized(http_req_t *req) {
  char tok[128];
  const char *h;

  if(!g_token[0]) {
    return 1;
  }
  if((h = http_header(req, "X-PS5AIO-Token")) && !strcmp(h, g_token)) {
    return 1;
  }
  if(http_query(req, "token", tok, sizeof(tok)) && !strcmp(tok, g_token)) {
    return 1;
  }
  return 0;
}

void
api_dispatch(http_req_t *req) {
  int path_matched = 0;

  if(strncmp(req->path, "/api/", 5) && strncmp(req->path, "/fs/", 4) &&
     strncmp(req->path, "/apps/", 6)) {
    webui_serve(req);
    return;
  }
  if(strcmp(req->path, "/api/ping") && !authorized(req)) {
    http_send_error(req, 401, "invalid or missing token");
    return;
  }

  for(size_t i = 0; i < sizeof(g_routes) / sizeof(g_routes[0]); i++) {
    const api_route_t *r = &g_routes[i];
    if(strcmp(r->path, req->path)) {
      continue;
    }
    path_matched = 1;
    if(r->method && strcmp(r->method, req->method) &&
       !(!strcmp(req->method, "HEAD") && !strcmp(r->method, "GET"))) {
      continue;
    }
    r->fn(req);
    return;
  }

  if(path_matched) {
    http_send_error(req, 405, "method %s not allowed on %s", req->method, req->path);
  } else {
    http_send_error(req, 404, "no such endpoint: %s", req->path);
  }
}
