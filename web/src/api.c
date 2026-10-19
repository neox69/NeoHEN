/* PS5AIO - REST API: router, shared helpers and system endpoints.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>

#include "fsutil.h"
#include "jobs.h"
#include "log.h"
#include "plat.h"
#include "zipstream.h"

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
  {
    /* NeoHEN writes its own version there at start (0.1.0-rc6 onwards) */
    char local[PATH_MAX], *text = NULL;
    size_t len = 0;

    plat_path("/data/neohen/version.txt", local, sizeof(local));
    if(!fs_read_file(local, &text, &len, 128)) {
      while(len && (text[len - 1] == '\n' || text[len - 1] == '\r' || text[len - 1] == ' ')) {
        text[--len] = 0;
      }
      jw_kstr(&w, "neohen", text);
      free(text);
    }
  }
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

/* ------------------------------------------------------------------ fan
 * NeoHEN itself drives the fan: it reads its config.ini again within a
 * second of a change and keeps the threshold applied. So this only edits
 * the two lines of the [cooling] section, leaving the rest of the file as
 * the user has it.
 *
 * The threshold is the temperature the console lets itself reach before
 * the fan speeds up; 77 is what the console does by itself. From here it
 * can be set between 50 and 80: lower is cooler and louder, and nothing
 * above the console's own figure is offered. */
#define NEOHEN_CONFIG "/data/neohen/config.ini"
#define FAN_MIN 50
#define FAN_MAX 80
#define FAN_STOCK 77

/* The value of `key` in [section], or "" if it is not there. */
static void
ini_get(const char *text, const char *section, const char *key, char *out, size_t size) {
  char head[64];
  const char *p;
  size_t klen = strlen(key);

  out[0] = 0;
  snprintf(head, sizeof(head), "[%s]", section);
  if(!(p = strstr(text, head))) {
    return;
  }
  for(p = strchr(p, '\n'); p; p = strchr(p, '\n')) {
    p++;
    if(*p == '[') {
      return;
    }
    if(!strncmp(p, key, klen) && p[klen] == '=') {
      size_t n = strcspn(p + klen + 1, "\r\n");
      snprintf(out, size, "%.*s", (int)n, p + klen + 1);
      return;
    }
  }
}

/* A copy of the text with `key` set in [section]; NULL if the section or
 * the key is not in the file (it is NeoHEN's file to lay out). */
static char *
ini_set(const char *text, const char *section, const char *key, const char *value) {
  char head[64];
  const char *p;
  size_t klen = strlen(key);

  snprintf(head, sizeof(head), "[%s]", section);
  if(!(p = strstr(text, head))) {
    return NULL;
  }
  for(p = strchr(p, '\n'); p; p = strchr(p, '\n')) {
    p++;
    if(*p == '[') {
      return NULL;
    }
    if(!strncmp(p, key, klen) && p[klen] == '=') {
      const char *old = p + klen + 1;
      size_t olen = strcspn(old, "\r\n"), pre = (size_t)(old - text);
      size_t total = pre + strlen(value) + strlen(old + olen) + 1;
      char *out = malloc(total);
      if(out) {
        memcpy(out, text, pre);
        snprintf(out + pre, total - pre, "%s%s", value, old + olen);
      }
      return out;
    }
  }
  return NULL;
}

static void
api_sys_fan_get(http_req_t *req) {
  char local[PATH_MAX], mode[48], value[16];
  char *text = NULL;
  size_t len = 0;
  jw_t w;

  plat_path(NEOHEN_CONFIG, local, sizeof(local));
  if(fs_read_file(local, &text, &len, 1 << 20)) {
    http_send_error(req, 404, "NeoHEN's config.ini is not there");
    return;
  }
  ini_get(text, "cooling", "fan_control", mode, sizeof(mode));
  ini_get(text, "cooling", "temperature_threshold_celsius", value, sizeof(value));
  free(text);
  api_begin(&w);
  jw_kbool(&w, "manual", !strcmp(mode, "temperature_threshold"));
  jw_kint(&w, "threshold", value[0] ? atoi(value) : FAN_STOCK);
  jw_kint(&w, "min", FAN_MIN);
  jw_kint(&w, "max", FAN_MAX);
  jw_kint(&w, "stock", FAN_STOCK);
  api_finish(req, &w);
}

/* POST /api/sys/fan {"manual":true,"threshold":70} | {"manual":false} */
static void
api_sys_fan_set(http_req_t *req) {
  char local[PATH_MAX], tmp[PATH_MAX], value[16];
  char *text = NULL, *a = NULL, *b = NULL;
  size_t len = 0;
  jval_t *body = api_body(req);

  if(!body) {
    return;
  }
  int manual = json_get_bool(body, "manual", 0);
  int threshold = (int)json_get_int(body, "threshold", FAN_STOCK);
  json_free(body);
  if(manual && (threshold < FAN_MIN || threshold > FAN_MAX)) {
    http_send_error(req, 400, "the threshold goes from %d to %d", FAN_MIN, FAN_MAX);
    return;
  }
  plat_path(NEOHEN_CONFIG, local, sizeof(local));
  if(fs_read_file(local, &text, &len, 1 << 20)) {
    http_send_error(req, 404, "NeoHEN's config.ini is not there");
    return;
  }
  a = ini_set(text, "cooling", "fan_control", manual ? "temperature_threshold" : "automatic");
  if(a && manual) {
    snprintf(value, sizeof(value), "%d", threshold);
    b = ini_set(a, "cooling", "temperature_threshold_celsius", value);
    free(a);
    a = b;
  }
  free(text);
  if(!a) {
    http_send_error(req, 500, "config.ini has no [cooling] settings to change");
    return;
  }
  /* written whole next to it, then put in its place: never half a file */
  snprintf(tmp, sizeof(tmp), "%s.neoweb", local);
  if(fs_write_file(tmp, a, strlen(a)) || rename(tmp, local)) {
    free(a);
    unlink(tmp);
    http_send_error(req, 500, "config.ini: %s", strerror(errno));
    return;
  }
  free(a);
  http_send_ok(req);
}

static void
api_sys_eject(http_req_t *req) {
  http_drain_body(req);
  if(plat_eject_disc()) {
    http_send_error(req, 500, "%s", plat_error());
    return;
  }
  http_send_ok(req);
}

/* Rest Mode keeps NeoHEN; a restart or a power off does not: the console
 * comes back as stock and the exploit has to be run again. */
static void
api_sys_power(http_req_t *req) {
  jval_t *b = api_body(req);
  char action[32];
  if(!b) {
    return;
  }
  snprintf(action, sizeof(action), "%s", json_get_str(b, "action", ""));
  json_free(b);
  if(strcmp(action, "rest") && strcmp(action, "reboot") && strcmp(action, "poweroff")) {
    http_send_error(req, 400, "unknown power action");
    return;
  }
  /* reply first: the console goes away right after */
  http_send_ok(req);
  usleep(300 * 1000);
  if(plat_power(action)) {
    LOG_ERR("power %s: %s", action, plat_error());
    plat_notify("NeoWEB: %s", plat_error());
  }
}

/* Asks NeoHEN (its control port, on this console) to restart the home menu:
 * that is what makes it read a theme's files again. NeoHEN refuses while a
 * game or app is open. */
static void
api_sys_menu(http_req_t *req) {
  const uint32_t frame[2] = {0x4F4E494Fu, 3};
  struct sockaddr_in addr;
  struct timeval tv = {3, 0};
  unsigned char reply = 0xff;
  int port = 9048;
  const char *env = getenv("NEOWEB_CTL_PORT"); /* the tests' stand-in */
  int s = socket(AF_INET, SOCK_STREAM, 0);

  if(env && atoi(env) > 0) {
    port = atoi(env);
  }
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if(s < 0) {
    http_send_error(req, 500, "socket: %s", strerror(errno));
    return;
  }
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if(connect(s, (struct sockaddr *)&addr, sizeof(addr)) ||
     send(s, frame, sizeof(frame), 0) != (ssize_t)sizeof(frame) ||
     recv(s, &reply, 1, 0) != 1) {
    close(s);
    http_send_error(req, 502, "NeoHEN does not answer on its control port");
    return;
  }
  close(s);
  if(reply == 2) {
    http_send_error(req, 409, "a game or app is open");
  } else if(reply) {
    http_send_error(req, 501, "this NeoHEN cannot restart the menu");
  } else {
    http_send_ok(req);
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

#ifdef NEOWEB_FULL
/* ---------------------------------------------------------------- report
 * Everything someone helping with a problem asks for, in one ZIP: NeoHEN's
 * logs and configuration, the themes service's diary, this service's own
 * log, and what the console is (model, firmware, sensors, volumes,
 * processes). No save data, no user names, nothing from the games. */
static void
report_text(zlist_t *z, const char *name, jw_t *w) {
  if(!w->oom && w->data) {
    char *text = strndup(w->data, w->len);
    if(text) {
      zlist_add(z, name, NULL, text, strlen(text), time(NULL));
      free(text);
    }
  }
  jw_free(w);
}

static void
api_sys_report(http_req_t *req) {
  static const char *files[][2] = {
    {"/data/neohen/NeoHEN.log", "NeoHEN.log"},
    {"/data/neohen/NeoHEN.log.1", "NeoHEN.log.1"},
    {"/data/neohen/NeoHEN_util_daemon.log", "NeoHEN_util_daemon.log"},
    {"/data/neohen/NeoHEN_crash.log", "NeoHEN_crash.log"},
    {"/data/neohen/config.ini", "config.ini"},
    {"/data/neohen/themes/neotheme.log", "themes/neotheme.log"},
    {"/data/neohen/themes/state.txt", "themes/state.txt"},
    {"/data/neohen/themes/active.txt", "themes/active.txt"},
    {"/data/neohen/themes/default.txt", "themes/default.txt"},
  };
  char name[64], local[PATH_MAX];
  zlist_t z = {0};
  time_t now = time(NULL);
  struct tm tm;
  jw_t w;

  for(size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    struct stat st;
    plat_path(files[i][0], local, sizeof(local));
    if(!stat(local, &st) && S_ISREG(st.st_mode) && st.st_size > 0) {
      zlist_add(&z, files[i][1], local, NULL, (uint64_t)st.st_size, st.st_mtime);
    }
  }
  jw_init(&w);
  jw_obj(&w);
  jw_kstr(&w, "neoweb", PS5AIO_VERSION);
  jw_kint(&w, "made", (int64_t)now);
  plat_sysinfo(&w);
  jw_key(&w, "mounts");
  plat_mounts(&w);
  jw_key(&w, "procs");
  plat_procs(&w);
  jw_obj_end(&w);
  report_text(&z, "console.json", &w);

  jw_init(&w);
  log_dump_json(&w, 0);
  report_text(&z, "neoweb-log.json", &w);

  if(!z.n) {
    zlist_free(&z);
    http_send_error(req, 500, "out of memory");
    return;
  }
  localtime_r(&now, &tm);
  strftime(name, sizeof(name), "NeoHEN-report-%Y%m%d-%H%M%S.zip", &tm);
  zip_send(req, &z, name);
  zlist_free(&z);
}
#endif /* NEOWEB_FULL */

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
 * A SAFE=1 build is the restricted set: it reads the console's state, can
 * launch or close a game, and manages the payloads in NeoHEN's own payloads
 * folder (upload, start, stop, delete, auto-start), by bare file name only.
 * The file manager, installer, memory tools, dumper and power routes are
 * compiled into the default build (NEOWEB_FULL), not into that one.
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
  {"GET",  "/api/sys/report",       api_sys_report},
  {"POST", "/api/sys/eject",        api_sys_eject},
  {"GET",  "/api/sys/fan",          api_sys_fan_get},
  {"POST", "/api/sys/fan",          api_sys_fan_set},
  {"POST", "/api/sys/browser",      api_sys_browser},
  {"POST", "/api/sys/menu",         api_sys_menu},

  {"GET",  "/api/trophies",         api_trophies},
  {"GET",  "/api/trophies/set",     api_trophies_set},
  {"GET",  "/api/trophies/icon",    api_trophies_icon},

  {"GET",  "/api/games/images",     api_games_images},
  {"POST", "/api/games/fix",        api_games_fix},
  {"POST", "/api/games/autofix",    api_games_autofix},
  {"POST", "/api/games/unfix",      api_games_unfix},
  {"POST", "/api/games/note",       api_games_note},
  {"POST", "/api/games/move",       api_games_move},
  {"POST", "/api/games/peek",       api_games_peek},

  {NULL,   "/api/smp",              api_smp},
  {"POST", "/api/usb/eject",        api_usb_eject},
  {"GET",  "/api/launcher",         api_launcher},
  {"GET",  "/api/remoteplay",       api_remoteplay},
  {NULL,   "/api/update/notice",    api_update_notice},
  {"POST", "/api/remoteplay",       api_remoteplay_request},
  {"POST", "/api/launcher/install", api_launcher_install},
  {"POST", "/api/launcher/remove",  api_launcher_remove},
  {"GET",  "/api/store",            api_store},
  {"POST", "/api/store/fetch",      api_store_fetch},
  {NULL,   "/api/store/auto",       api_store_auto},
  {"GET",  "/api/cheats/live",      api_cheats_live},
  {"POST", "/api/cheats/toggle",    api_cheats_toggle},
  {"GET",  "/api/stats/playtime",   api_stats_playtime},
  {"GET",  "/api/library/status",   api_library_status_get},
  {"POST", "/api/library/status",   api_library_status_set},

  {"GET",  "/api/captures/list",    api_captures_list},
  {"GET",  "/api/captures/zip",     api_captures_zip},

  {"GET",  "/api/saves/list",       api_saves_list},
  {"POST", "/api/saves/backup",     api_saves_backup},
  {"GET",  "/api/saves/zip",        api_saves_zip},
  {"GET",  "/api/saves/backups",    api_saves_backups},
  {"POST", "/api/saves/restore",    api_saves_restore},
  {"POST", "/api/saves/import",     api_saves_import},

  {"GET",  "/api/jobs",             api_jobs_list},
  {"POST", "/api/jobs/cancel",      api_jobs_cancel},
  {"POST", "/api/jobs/clear",       api_jobs_clear},

  {"GET",  "/api/fs/list",          api_fs_list},
  {"GET",  "/api/fs/stat",          api_fs_stat},
  {"GET",  "/api/fs/walk",          api_fs_walk},
  {"GET",  "/api/fs/search",        api_fs_search},
  {"GET",  "/api/fs/size",          api_fs_size},
  {"GET",  "/api/fs/text",          api_fs_read_text},
  {"POST", "/api/fs/text",          api_fs_write_text},
  {"POST", "/api/fs/mkdir",         api_fs_mkdir},
  {"POST", "/api/fs/delete",        api_fs_delete},
  {"POST", "/api/fs/rename",        api_fs_rename},
  {"POST", "/api/fs/copy",          api_fs_copy},
  {"POST", "/api/fs/chmod",         api_fs_chmod},
  {"POST", "/api/fs/hash",          api_fs_hash},
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
  api_stats_init();
  api_launcher_init();
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
    /* One switch for the whole service: with the marker file present,
     * nothing that changes the console is served. */
    if(strcmp(req->method, "GET") && strcmp(req->method, "HEAD")) {
      char marker[1024];
      plat_path(PS5AIO_DATA_DIR "/readonly", marker, sizeof(marker));
      if(!access(marker, F_OK)) {
        http_send_error(req, 403, "NeoWEB is read-only (%s exists)",
                        PS5AIO_DATA_DIR "/readonly");
        return;
      }
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
