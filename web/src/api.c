/* PS5AIO - REST API: router, shared helpers and system endpoints.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>

#include "fsutil.h"
#include "jobs.h"
#include "log.h"
#include "plat.h"
#include "sha256.h"
#include "zipstream.h"

/* Optional shared secrets from config.json: "token" gives every route,
 * "guestToken" only the read-only routes of guest_allowed(). Changed at run
 * time from the page (api_auth_*), so read and written under g_tok_lock. */
static char g_token[128];
static char g_guest[128];
static pthread_mutex_t g_tok_lock = PTHREAD_MUTEX_INITIALIZER;

enum { ROLE_NONE, ROLE_GUEST, ROLE_ADMIN };

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
  /* a path cut short by the buffer would name another file */
  if(strlen(local) >= size - 1) {
    http_send_error(req, 400, "path too long");
    return -1;
  }
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

static int request_role(http_req_t *req, int *by_cookie);
static int same_origin(http_req_t *req);

/* Send `w` with the page's cookie set to `tok` ("" takes it away; NULL
 * leaves it). HttpOnly: a page on another port of the console sees the
 * cookie's requests go by but cannot read the token out of it. */
static void
send_with_cookie(http_req_t *req, jw_t *w, const char *tok) {
  char extra[400];
  size_t o;

  jw_obj_end(w);
  if(!tok || w->oom) {
    http_send_json(req, 200, w);
    jw_free(w);
    return;
  }
  o = (size_t)snprintf(extra, sizeof(extra), "Set-Cookie: nwtok=");
  for(const unsigned char *c = (const unsigned char *)tok; *c && o < sizeof(extra) - 100; c++) {
    o += isalnum(*c) || strchr("._~-", *c) ? (size_t)snprintf(extra + o, 2, "%c", *c)
                                           : (size_t)snprintf(extra + o, 4, "%%%02X", *c);
  }
  snprintf(extra + o, sizeof(extra) - o, "; Path=/; HttpOnly; SameSite=Strict; Max-Age=%d\r\n",
           tok[0] ? 31536000 : 0);
  http_send_buf_ex(req, 200, "application/json; charset=utf-8", w->data, w->len, extra);
  jw_free(w);
}

/* Open to anyone (the watchdog and the page ask it before they have a
 * token): says whether a token is needed and what the one sent gives. A
 * token sent in the header or as ?token= by NeoWEB's own page becomes its
 * cookie (images and downloads carry it); a wrong one takes the cookie away. */
static void
api_ping(http_req_t *req) {
  static const char *const roles[] = {"none", "guest", "admin"};
  int has_token, has_guest, cookie;
  int role = request_role(req, &cookie);
  const char *h = http_header(req, "X-PS5AIO-Token");
  char sent[128] = "";
  jw_t w;

  if(h) {
    snprintf(sent, sizeof(sent), "%s", h);
  } else {
    http_query(req, "token", sent, sizeof(sent));
  }

  pthread_mutex_lock(&g_tok_lock);
  has_token = g_token[0] != 0;
  has_guest = g_token[0] && g_guest[0];
  pthread_mutex_unlock(&g_tok_lock);
  api_begin(&w);
  jw_kstr(&w, "name", PS5AIO_NAME);
  jw_kstr(&w, "version", PS5AIO_VERSION);
  jw_kstr(&w, "neohenBuild", NEOHEN_VERSION);
  jw_kstr(&w, "platform", plat_name());
  jw_kbool(&w, "auth", has_token);
  jw_kbool(&w, "guestLink", has_guest);
  jw_kstr(&w, "role", roles[role]);
#ifdef NEOWEB_FULL
  jw_kbool(&w, "full", 1);
#else
  jw_kbool(&w, "full", 0);
#endif
  if(!cookie && (h || sent[0]) && has_token && same_origin(req)) {
    send_with_cookie(req, &w, role == ROLE_NONE ? "" : sent);
  } else {
    api_finish(req, &w);
  }
}

static void
api_sys_info(http_req_t *req) {
  jw_t w;
  api_begin(&w);
  jw_kstr(&w, "version", PS5AIO_VERSION);
  /* the NeoHEN release this NeoWEB was built with; "neohen" below is the
   * NeoHEN that is running */
  jw_kstr(&w, "neohenBuild", NEOHEN_VERSION);
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
 * logs and crash logs, its configuration (tokens and passwords taken out),
 * the themes service's diary, this service's own log, what the console is
 * (model, firmware, sensors, volumes, processes), safe mode and conflicting
 * homebrew, and every payload with its version and SHA-256. No save data,
 * no user names, nothing from the games. */

/* a setting whose value is not for sharing */
static int
secret_key(const char *k, size_t n) {
  static const char *const words[] = {"token", "pass", "secret", "key", "auth", "pin", "cookie", "account"};
  char low[64];
  size_t i;

  for(i = 0; i < n && i < sizeof(low) - 1; i++) {
    low[i] = (char)tolower((unsigned char)k[i]);
  }
  low[i] = 0;
  for(i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
    if(strstr(low, words[i])) {
      return 1;
    }
  }
  return 0;
}

static void
scrub_json(jval_t *v) {
  for(; v; v = v->next) {
    if(v->key && v->type == J_STR && v->str && v->str[0] && secret_key(v->key, strlen(v->key))) {
      free(v->str);
      v->str = strdup("(removed)");
    }
    if(v->type == J_OBJ || v->type == J_ARR) {
      scrub_json(v->child);
    }
  }
}

/* an INI (key=value lines) or JSON file, with the secret values taken out */
static void
report_scrubbed(zlist_t *z, const char *console, const char *name) {
  char local[PATH_MAX];
  char *data = NULL, *out = NULL;
  size_t len = 0, lines = 1, o = 0;
  struct stat st;

  plat_path(console, local, sizeof(local));
  if(stat(local, &st) || fs_read_file(local, &data, &len, 1 << 20)) {
    return;
  }
  if(data[0] == '{') {
    jval_t *v = json_parse(data, len);
    jw_t w;
    jw_init(&w);
    if(v) {
      scrub_json(v->child);
      json_write(&w, v);
      json_free(v);
    }
    if(!w.oom && w.data && (out = strndup(w.data, w.len))) {
      zlist_add(z, name, NULL, out, strlen(out), st.st_mtime);
      free(out);
    }
    jw_free(&w);
    free(data);
    return;
  }
  for(size_t i = 0; i < len; i++) {
    lines += data[i] == '\n';
  }
  /* each line may grow by "(removed)" at most */
  if(!(out = malloc(len + lines * 10 + 1))) {
    free(data);
    return;
  }
  for(char *line = data; line < data + len;) {
    char *nl = memchr(line, '\n', (size_t)(data + len - line));
    size_t n = nl ? (size_t)(nl - line) + 1 : (size_t)(data + len - line);
    char *eq = memchr(line, '=', n);
    if(line[0] != '#' && line[0] != ';' && eq && secret_key(line, (size_t)(eq - line))) {
      size_t keep = (size_t)(eq - line) + 1;
      memcpy(out + o, line, keep);
      o += keep;
      memcpy(out + o, "(removed)", 9);
      o += 9;
      if(nl) {
        out[o++] = '\n';
      }
    } else {
      memcpy(out + o, line, n);
      o += n;
    }
    line += n;
  }
  out[o] = 0;
  zlist_add(z, name, NULL, out, o, st.st_mtime);
  free(out);
  free(data);
}
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

/* NeoHEN: how fast the network is between this browser and the console, as
 * PS5Upload's "Diagnose slow transfers". GET /api/net/down?mb=N sends N MiB
 * of zeros (at most 1024); POST /api/net/up reads the body and says how long
 * that took on this side. Nothing touches a drive. */
static void
api_net_down(http_req_t *req) {
  static char zero[256 * 1024];
  int64_t mb = http_query_int(req, "mb", 64);
  if(mb < 1) {
    mb = 1;
  } else if(mb > 1024) {
    mb = 1024;
  }
  int64_t total = mb * 1024 * 1024;
  http_send_head(req, 200, "application/octet-stream", total,
                 "Cache-Control: no-store\r\n");
  for(int64_t sent = 0; sent < total; sent += sizeof(zero)) {
    if(http_send_all(req->fd, zero, sizeof(zero))) {
      break;
    }
  }
}

static void
api_net_up(http_req_t *req) {
  const size_t cap = 256 * 1024;
  char *sink = malloc(cap);
  struct timeval t0, t1;
  int64_t got = 0;
  ssize_t n;
  jw_t w;

  if(!sink) {
    http_send_error(req, 500, "out of memory");
    return;
  }
  gettimeofday(&t0, NULL);
  while((n = http_read_body(req, sink, cap)) > 0) {
    got += n;
  }
  gettimeofday(&t1, NULL);
  free(sink);
  jw_init(&w);
  jw_obj(&w);
  jw_kbool(&w, "ok", 1);
  jw_kint(&w, "bytes", got);
  jw_kint(&w, "ms", (int64_t)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000);
  jw_obj_end(&w);
  http_send_json(req, 200, &w);
  jw_free(&w);
}

static void
api_sys_report(http_req_t *req) {
  static const char *files[][2] = {
    {"/data/neohen/NeoHEN.log", "NeoHEN.log"},
    {"/data/neohen/NeoHEN.log.1", "NeoHEN.log.1"},
    {"/data/neohen/NeoHEN_util_daemon.log", "NeoHEN_util_daemon.log"},
    {"/data/neohen/NeoHEN_crash.log", "NeoHEN_crash.log"},
    {PS5AIO_DATA_DIR "/neoweb.log", "neoweb.log"},
    {PS5AIO_DATA_DIR "/neoweb.log.1", "neoweb.log.1"},
    {"/data/neohen/version.txt", "version.txt"},
    {"/data/neohen/safe_mode", "safe_mode.txt"},
    {"/data/neohen/boot_attempts", "boot_attempts.txt"},
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
  jw_key(&w, "safeMode");
  jw_obj(&w);
  api_health_safe_mode(&w);
  jw_obj_end(&w);
  jw_key(&w, "conflicts");
  api_health_conflicts(&w);
  jw_obj_end(&w);
  report_text(&z, "console.json", &w);

  /* every payload with its SHA-256 and where it came from */
  jw_init(&w);
  api_payloads_manifest(&w);
  report_text(&z, "payloads.json", &w);

  report_scrubbed(&z, "/data/neohen/config.ini", "config.ini");
  report_scrubbed(&z, PS5AIO_DATA_DIR "/config.json", "neoweb-config.json");
  report_scrubbed(&z, PS5AIO_DATA_DIR "/store.json", "store.json");
  report_scrubbed(&z, PS5AIO_DATA_DIR "/apps.json", "apps.json");
  /* NeoHEN installed from NeoWEB: trial, rollback (docs/robustness.md) */
  report_scrubbed(&z, "/data/neohen/update/state.json", "update-state.json");

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

/* ------------------------------------------------------------ access
 * config.json may hold "token" (every route) and "guestToken" (the
 * read-only routes of guest_allowed()). A guest token only means something
 * next to a token: without one anybody on the network has every route. The
 * page asks for both here, so they are kept in config.json with whatever
 * else the user put there. */
#define NEOWEB_CONFIG PS5AIO_DATA_DIR "/config.json"

static void
token_new(char out[33]) {
  unsigned char raw[16];
  int fd = open("/dev/urandom", O_RDONLY);
  ssize_t got = fd >= 0 ? read(fd, raw, sizeof(raw)) : -1;

  if(fd >= 0) {
    close(fd);
  }
  if(got != (ssize_t)sizeof(raw)) {
    /* no urandom: the clock, the stack and a counter, through SHA-256 */
    static unsigned counter;
    struct timespec ts[2];
    sha256_t s;
    char hex[65];
    clock_gettime(CLOCK_REALTIME, &ts[0]);
    clock_gettime(CLOCK_MONOTONIC, &ts[1]);
    unsigned c = __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED);
    void *where = &s;
    pid_t pid = getpid();
    sha256_init(&s);
    sha256_update(&s, ts, sizeof(ts));
    sha256_update(&s, &c, sizeof(c));
    sha256_update(&s, &where, sizeof(where));
    sha256_update(&s, &pid, sizeof(pid));
    sha256_final(&s, hex);
    memcpy(out, hex, 32);
    out[32] = 0;
    return;
  }
  for(int i = 0; i < 16; i++) {
    snprintf(out + i * 2, 3, "%02x", raw[i]);
  }
}

/* Set "token" and "guestToken" in config.json (NULL leaves one as it is),
 * keeping the rest of the file; then use them. */
static int
tokens_save(http_req_t *req, const char *token, const char *guest) {
  char local[PATH_MAX], tmp[PATH_MAX + 8];
  char *text = NULL;
  size_t len = 0;
  jval_t *cfg;
  jw_t w;
  int rc = -1;

  plat_path(NEOWEB_CONFIG, local, sizeof(local));
  if(fs_read_file(local, &text, &len, 64 * 1024)) {
    cfg = json_parse("{}", 2);
  } else {
    cfg = json_parse(text, len);
    free(text);
    if(!cfg || cfg->type != J_OBJ) {
      json_free(cfg);
      http_send_error(req, 409, "%s is not valid JSON: fix it by hand first", NEOWEB_CONFIG);
      return -1;
    }
  }
  if(!cfg || (token && !json_set_str(cfg, "token", token)) ||
     (guest && !json_set_str(cfg, "guestToken", guest))) {
    json_free(cfg);
    http_send_error(req, 500, "out of memory");
    return -1;
  }
  jw_init(&w);
  json_write(&w, cfg);
  json_free(cfg);
  snprintf(tmp, sizeof(tmp), "%s.tmp", local);
  fs_mkparents(local);
  if(w.oom || !w.data || fs_write_file(tmp, w.data, w.len) || rename(tmp, local)) {
    unlink(tmp);
    http_send_error(req, 500, "%s: %s", NEOWEB_CONFIG, strerror(errno));
  } else {
    pthread_mutex_lock(&g_tok_lock);
    if(token) {
      snprintf(g_token, sizeof(g_token), "%s", token);
    }
    if(guest) {
      snprintf(g_guest, sizeof(g_guest), "%s", guest);
    }
    pthread_mutex_unlock(&g_tok_lock);
    rc = 0;
  }
  jw_free(&w);
  return rc;
}

/* GET /api/auth (the token's own route): whether there is a token, and the
 * guest token, to make the guest link of */
static void
api_auth_get(http_req_t *req) {
  char guest[128];
  int has;
  jw_t w;

  pthread_mutex_lock(&g_tok_lock);
  has = g_token[0] != 0;
  snprintf(guest, sizeof(guest), "%s", has ? g_guest : "");
  pthread_mutex_unlock(&g_tok_lock);
  api_begin(&w);
  jw_kbool(&w, "token", has);
  jw_kstr(&w, "guest", guest);
  api_finish(req, &w);
}

/* POST /api/auth/token {"action":"new"|"clear"}: a new token comes back in
 * the answer, the only time it is shown; clearing it ends the guest token too */
static void
api_auth_token(http_req_t *req) {
  char action[16], tok[33] = "";
  jval_t *b = api_body(req);
  jw_t w;

  if(!b) {
    return;
  }
  snprintf(action, sizeof(action), "%s", json_get_str(b, "action", ""));
  json_free(b);
  if(!strcmp(action, "new")) {
    token_new(tok);
    if(tokens_save(req, tok, NULL)) {
      return;
    }
    LOG_INFO("a new API token was made from %s", req->client_ip);
  } else if(!strcmp(action, "clear")) {
    if(tokens_save(req, "", "")) {
      return;
    }
    LOG_INFO("API token removed from %s", req->client_ip);
  } else {
    http_send_error(req, 400, "action is new or clear");
    return;
  }
  api_begin(&w);
  jw_kstr(&w, "token", tok);
  send_with_cookie(req, &w, tok);
}

/* POST /api/auth/guest {"action":"new"|"revoke"} */
static void
api_auth_guest(http_req_t *req) {
  char action[16], tok[33] = "";
  int has;
  jval_t *b = api_body(req);
  jw_t w;

  if(!b) {
    return;
  }
  snprintf(action, sizeof(action), "%s", json_get_str(b, "action", ""));
  json_free(b);
  pthread_mutex_lock(&g_tok_lock);
  has = g_token[0] != 0;
  pthread_mutex_unlock(&g_tok_lock);
  if(!strcmp(action, "new")) {
    if(!has) {
      http_send_error(req, 409, "set a token first: without one everybody has every route");
      return;
    }
    token_new(tok);
    if(tokens_save(req, NULL, tok)) {
      return;
    }
  } else if(!strcmp(action, "revoke")) {
    if(tokens_save(req, NULL, "")) {
      return;
    }
  } else {
    http_send_error(req, 400, "action is new or revoke");
    return;
  }
  api_begin(&w);
  jw_kstr(&w, "guest", tok);
  api_finish(req, &w);
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
  {"GET",  "/api/health",             api_health},
  {"POST", "/api/health/safemode",    api_health_safe_mode_clear},
  {"GET",  "/api/auth",               api_auth_get},
  {"POST", "/api/auth/token",         api_auth_token},
  {"POST", "/api/auth/guest",         api_auth_guest},
  {"GET",  "/api/homebrew/list",      api_homebrew_list},
  {"GET",  "/api/homebrew/icon",      api_homebrew_icon},

#ifdef NEOWEB_FULL
  {"POST", "/api/sys/kill",         api_sys_kill},
  {"POST", "/api/sys/power",        api_sys_power},
  {"GET",  "/api/sys/report",       api_sys_report},
  {"POST", "/api/sys/eject",        api_sys_eject},
  {"GET",  "/api/sys/fan",          api_sys_fan_get},
  {"POST", "/api/sys/fan",          api_sys_fan_set},
  {"GET",  "/api/neohen/settings",   api_neohen_settings_get},
  {"POST", "/api/neohen/settings",   api_neohen_settings_set},
  {"GET",  "/api/neohen/backup",     api_neohen_backup},
  {"POST", "/api/neohen/restore",    api_neohen_restore},
  {"POST", "/api/sys/browser",      api_sys_browser},
  {"POST", "/api/sys/menu",         api_sys_menu},
  {"GET",  "/api/net/down",         api_net_down},
  {"POST", "/api/net/up",           api_net_up},

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
  {NULL,   "/api/update",           api_update},
  {NULL,   "/api/dns/updates",      api_dns_updates},
  {"POST", "/api/remoteplay",       api_remoteplay_request},
  {"POST", "/api/launcher/install", api_launcher_install},
  {"POST", "/api/launcher/remove",  api_launcher_remove},
  {"GET",  "/api/store",            api_store},
  {"POST", "/api/store/fetch",      api_store_fetch},
  {NULL,   "/api/store/auto",       api_store_auto},
  {NULL,   "/api/apps/store",        api_apps_store},
  {"GET",  "/api/cheats/live",      api_cheats_live},
  {"POST", "/api/cheats/toggle",    api_cheats_toggle},
  {"GET",  "/api/stats/playtime",   api_stats_playtime},
  {"GET",  "/api/library/status",   api_library_status_get},
  {"POST", "/api/library/status",   api_library_status_set},
  {"GET",  "/api/library/cleanup",  api_library_cleanup},
  {"GET",  "/api/homebrew/js",      api_homebrew_js},
  {"GET",  "/api/homebrew/ls",      api_homebrew_ls},
  {"POST", "/api/homebrew/launch",  api_homebrew_launch},

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
  {"POST", "/api/fs/extract",       api_fs_extract},
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
  jval_t *cfg;

  plat_path(NEOWEB_CONFIG, path, sizeof(path));
  if(fs_read_file(path, &data, &len, 64 * 1024)) {
    cfg = json_parse("{}", 2);
  } else {
    cfg = json_parse(data, len);
    free(data);
  }
  if(!cfg) {
    LOG_WARN("config.json is not valid JSON");
    return;
  }
  pthread_mutex_lock(&g_tok_lock);
  snprintf(g_token, sizeof(g_token), "%s", json_get_str(cfg, "token", ""));
  snprintf(g_guest, sizeof(g_guest), "%s", json_get_str(cfg, "guestToken", ""));
  pthread_mutex_unlock(&g_tok_lock);
  json_free(cfg);
  if(g_token[0]) {
    LOG_INFO("API token authentication enabled%s", g_guest[0] ? ", with a guest token" : "");
  }
}

void
api_tokens_reload(void) {
  load_config();
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

/* the same in a time that does not depend on where they differ */
static int
tok_eq(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  unsigned char d = la != lb;
  for(size_t i = 0; i < la && i < lb; i++) {
    d |= (unsigned char)(a[i] ^ b[i]);
  }
  return !d;
}

/* the value of cookie `name`, url-decoded, or 0 */
static int
cookie_get(http_req_t *req, const char *name, char *out, size_t size) {
  const char *c = http_header(req, "Cookie");
  size_t n = strlen(name);

  for(; c && *c; c += strcspn(c, ";"), c += *c == ';') {
    c += strspn(c, " \t");
    if(!strncmp(c, name, n) && c[n] == '=') {
      url_decode(c + n + 1, strcspn(c + n + 1, ";"), out, size);
      return 1;
    }
  }
  return 0;
}

/* What the request may do: with no token set, everything; otherwise what
 * the token it carries gives, from the X-PS5AIO-Token header, ?token= or
 * the page's nwtok cookie, in that order. A cookie goes with any request
 * the browser makes, so *by_cookie says the caller has to check where the
 * request comes from. */
static int
request_role(http_req_t *req, int *by_cookie) {
  char tok[128], admin[128], guest[128];
  const char *h;

  *by_cookie = 0;
  pthread_mutex_lock(&g_tok_lock);
  snprintf(admin, sizeof(admin), "%s", g_token);
  snprintf(guest, sizeof(guest), "%s", g_guest);
  pthread_mutex_unlock(&g_tok_lock);
  if(!admin[0]) {
    return ROLE_ADMIN;
  }
  if((h = http_header(req, "X-PS5AIO-Token"))) {
    snprintf(tok, sizeof(tok), "%s", h);
  } else if(!http_query(req, "token", tok, sizeof(tok)) || !tok[0]) {
    if(!cookie_get(req, "nwtok", tok, sizeof(tok)) || !tok[0]) {
      return ROLE_NONE;
    }
    *by_cookie = 1;
  }
  if(tok_eq(tok, admin)) {
    return ROLE_ADMIN;
  }
  if(guest[0] && tok_eq(tok, guest)) {
    return ROLE_GUEST;
  }
  return ROLE_NONE;
}

/* What a guest token may use: an explicit list (a route not named here,
 * new ones included, is refused), only to read, and nothing that hands out
 * files by path except the console's captures. No settings, tokens, logs,
 * file manager, memory, payload start/stop, installs, power or reports. */
static int
guest_allowed(http_req_t *req) {
  static const char *const reads[] = {
    "/api/ping", "/api/sys/info", "/api/sys/sensors", "/api/sys/mounts",
    "/api/apps/list", "/api/apps/param", "/apps/icon", "/api/apps/running",
    "/api/payloads/list", "/api/health",
    "/api/homebrew/list", "/api/homebrew/icon",
    "/api/trophies", "/api/trophies/set", "/api/trophies/icon",
    "/api/games/images", "/api/stats/playtime", "/api/library/status",
    "/api/captures/list",
  };
  char path[1024];

  if(strcmp(req->method, "GET") && strcmp(req->method, "HEAD")) {
    return 0;
  }
  for(size_t i = 0; i < sizeof(reads) / sizeof(reads[0]); i++) {
    if(!strcmp(req->path, reads[i])) {
      return 1;
    }
  }
  /* the screenshots and clips themselves, and their thumbnails */
  if(!strcmp(req->path, "/fs/get") && http_query(req, "path", path, sizeof(path)) &&
     !strncmp(path, "/user/av_contents/", 18) && !strstr(path, "..") && !strstr(path, "//")) {
    return 1;
  }
  return 0;
}

/* The host part of a Host header or of an origin's authority: "name",
 * "name:port", "[v6]" or "[v6]:port". */
static void
host_part(const char *in, char *out, size_t size) {
  size_t n;
  if(in[0] == '[') {
    const char *close = strchr(in, ']');
    n = close ? (size_t)(close - in) + 1 : strlen(in);
  } else {
    n = strcspn(in, ":");
  }
  snprintf(out, size, "%.*s", (int)n, in);
}

static int
ends_with_ci(const char *s, const char *tail) {
  size_t a = strlen(s), b = strlen(tail);
  return a >= b && !strcasecmp(s + a - b, tail);
}

/* A name the console can have on a home network: an IP address, localhost,
 * a bare name or one of the local suffixes. Anything else is a public name,
 * which is what a DNS rebinding attack points at the console. */
static int
local_host(const char *host) {
  static const char *const suffixes[] = {".local", ".lan", ".home", ".internal", ".home.arpa"};
  char h[256];
  size_t i;

  host_part(host, h, sizeof(h));
  if(!h[0] || h[0] == '[' || !strchr(h, '.')) {
    return 1;
  }
  for(i = 0; h[i] && (isdigit((unsigned char)h[i]) || h[i] == '.'); i++) {
  }
  if(!h[i]) {
    return 1;
  }
  for(i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
    if(ends_with_ci(h, suffixes[i])) {
      return 1;
    }
  }
  return 0;
}

/* Without a token anyone on the network may use the API, but a web page
 * from somewhere else must not: browsers send an Origin header with every
 * cross-origin request that can change something, and it has to be this
 * very server. Programs that are not browsers send no Origin at all. */
static int
same_origin(http_req_t *req) {
  const char *host = http_header(req, "Host");
  const char *origin = http_header(req, "Origin");

  if(host && !local_host(host)) {
    return 0;
  }
  if(!origin) {
    return 1;
  }
  return host && !strncasecmp(origin, "http://", 7) && !strcasecmp(origin + 7, host);
}

/* Whether the readonly marker is there. Asked on every request that is not a
 * GET, so the answer is kept for a couple of seconds: creating or removing
 * the file takes that long to count here (NeoHEN's payload routes still look
 * for it themselves each time). */
#define READONLY_TTL_MS 2000

static int
read_only_marker(void) {
  static int64_t checked = INT64_MIN / 2;
  static int ro;
  struct timespec ts;
  int64_t now;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  if(now - __atomic_load_n(&checked, __ATOMIC_ACQUIRE) < READONLY_TTL_MS) {
    return __atomic_load_n(&ro, __ATOMIC_RELAXED);
  }
  char marker[1024];
  plat_path(PS5AIO_DATA_DIR "/readonly", marker, sizeof(marker));
  __atomic_store_n(&ro, !access(marker, F_OK), __ATOMIC_RELAXED);
  __atomic_store_n(&checked, now, __ATOMIC_RELEASE);
  return ro;
}

void
api_dispatch(http_req_t *req) {
  int path_matched = 0;

  if(strncmp(req->path, "/api/", 5) && strncmp(req->path, "/fs/", 4) &&
     strncmp(req->path, "/apps/", 6)) {
    webui_serve(req);
    return;
  }
  int by_cookie, has_token;
  int role = request_role(req, &by_cookie);

  pthread_mutex_lock(&g_tok_lock);
  has_token = g_token[0] != 0;
  pthread_mutex_unlock(&g_tok_lock);
  if(strcmp(req->path, "/api/ping") && role == ROLE_NONE) {
    http_send_error(req, 401, "invalid or missing token");
    return;
  }
  /* with a token, a page elsewhere cannot get in anyway: it does not know
   * it; but the browser sends the page's cookie to anybody's request */
  if((!has_token || by_cookie) && !same_origin(req)) {
    http_send_error(req, 403, "cross-origin request refused (open NeoWEB at the "
                    "console's address, or set a token in config.json)");
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
    if(strcmp(req->method, "GET") && strcmp(req->method, "HEAD") && read_only_marker()) {
      http_send_error(req, 403, "NeoWEB is read-only (%s exists)",
                      PS5AIO_DATA_DIR "/readonly");
      return;
    }
    if(role == ROLE_GUEST && !guest_allowed(req)) {
      http_send_error(req, 403, "the guest link can only look, not change or download things");
      return;
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
