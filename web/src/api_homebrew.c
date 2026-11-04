/* NeoWEB - homebrew laid out the way ps5-payload-dev/websrv expects it.
 *
 * websrv (https://github.com/ps5-payload-dev/websrv, assets/localStore.js)
 * looks in these folders:
 *
 *   /data/homebrew/  /mnt/usb0..6/homebrew/  /mnt/ext0..1/homebrew/
 *
 * and takes every folder in them that holds homebrew.js or eboot.elf
 * (homebrew.js first when both are there). The icon is sce_sys/icon0.png.
 * websrv itself never reads a param.json; when sce_sys/param.json is there
 * its titleName is shown here, otherwise the folder's name.
 *
 * eboot.elf alone is launched with no arguments. homebrew.js is a script
 * that builds a menu and, when something in it is picked, says what to run:
 * {path, args, env, cwd, daemon}. websrv runs that through its own /hbldr
 * (the ELF in a fake app, with argv, environment and working directory);
 * NeoHEN's ELF loader on port 9021 only takes the ELF, so:
 *
 *   - no arguments, environment or working directory: the ELF goes to the
 *     ELF loader on 9021, like a payload started from the Payloads section;
 *   - any of them: NeoWEB asks websrv, when it is running on 127.0.0.1:8080,
 *     to launch it exactly the way it would (GET /hbldr?path=&args=...).
 *     Without websrv that launch is refused with a reason.
 *
 * Routes (the listing and icons in both builds, the rest in the full one):
 *   GET  /api/homebrew/list
 *   GET  /api/homebrew/icon?dir=/data/homebrew/Foo
 *   GET  /api/homebrew/js?dir=...                    the text of homebrew.js
 *   GET  /api/homebrew/ls?dir=...&path=...           a folder inside the app
 *   POST /api/homebrew/launch {"dir", "path"?, "args"?, "env"?, "cwd"?,
 *                              "daemon"?}
 *
 * The page runs homebrew.js in a sandboxed iframe (scripts only, no same
 * origin): it cannot call NeoWEB, it can only ask the page by message.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "fsutil.h"
#include "log.h"
#include "plat.h"

#define HB_JS_MAX   (1 << 20)
#define HB_ARGS_MAX 4096
#define HB_MAX_APPS 256
#define WEBSRV_PORT 8080

/* the same list as websrv's HOMEBREW_AUTO_SCAN_PATHS */
static const char *const HB_ROOTS[] = {
  "/data/homebrew",
  "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew",
  "/mnt/usb3/homebrew", "/mnt/usb4/homebrew", "/mnt/usb5/homebrew",
  "/mnt/usb6/homebrew",
  "/mnt/ext0/homebrew", "/mnt/ext1/homebrew",
};
#define HB_NROOTS (sizeof(HB_ROOTS) / sizeof(HB_ROOTS[0]))

static int
file_kind(const char *console, struct stat *st) {
  char local[PATH_MAX];
  plat_path(console, local, sizeof(local));
  return stat(local, st) ? -1 : 0;
}

static int
is_file(const char *console) {
  struct stat st;
  return !file_kind(console, &st) && S_ISREG(st.st_mode);
}

static int
is_folder(const char *console) {
  struct stat st;
  return !file_kind(console, &st) && S_ISDIR(st.st_mode);
}

/* a single path component: no slash, not "." or "..", not hidden */
static int
plain_name(const char *s, size_t n) {
  if(!n || s[0] == '.' || n > 255) {
    return 0;
  }
  for(size_t i = 0; i < n; i++) {
    if(s[i] == '/' || s[i] == '\\' || (unsigned char)s[i] < 0x20) {
      return 0;
    }
  }
  return 1;
}

/* a console path made of plain components only: no "..", "." or "//" */
static int
clean_abs(const char *p) {
  if(p[0] != '/') {
    return 0;
  }
  for(const char *c = p + 1; *c;) {
    size_t n = strcspn(c, "/");
    if(!n || (n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.')) {
      return 0;
    }
    for(size_t i = 0; i < n; i++) {
      if((unsigned char)c[i] < 0x20) {
        return 0;
      }
    }
    c += n;
    if(*c == '/') {
      c++;
      if(!*c) {
        return 0;
      }
    }
  }
  return 1;
}

/* dir is "<one of HB_ROOTS>/<name>" and a homebrew folder (eboot.elf or
 * homebrew.js in it); 0 when it is */
static int
hb_dir_ok(const char *dir) {
  char path[PATH_MAX];

  for(size_t i = 0; i < HB_NROOTS; i++) {
    size_t n = strlen(HB_ROOTS[i]);
    if(strncmp(dir, HB_ROOTS[i], n) || dir[n] != '/' ||
       !plain_name(dir + n + 1, strlen(dir + n + 1))) {
      continue;
    }
    if(!is_folder(dir)) {
      return -1;
    }
    snprintf(path, sizeof(path), "%s/eboot.elf", dir);
    if(is_file(path)) {
      return 0;
    }
    snprintf(path, sizeof(path), "%s/homebrew.js", dir);
    return is_file(path) ? 0 : -1;
  }
  return -1;
}

/* the app's folder from ?dir=, replying 400/404 when it is not one */
static int
query_dir(http_req_t *req, char *dir, size_t size) {
  if(!http_query(req, "dir", dir, size) || !dir[0]) {
    http_send_error(req, 400, "missing 'dir'");
    return -1;
  }
  if(hb_dir_ok(dir)) {
    http_send_error(req, 404, "%s is not a homebrew folder", dir);
    return -1;
  }
  return 0;
}

/* titleName from sce_sys/param.json, the way the console's own files say it */
static void
param_title(const char *dir, char *out, size_t size) {
  char console[PATH_MAX], local[PATH_MAX];
  char *data = NULL;
  size_t len = 0;
  jval_t *pj, *lp;
  const char *t = NULL;

  out[0] = 0;
  snprintf(console, sizeof(console), "%s/sce_sys/param.json", dir);
  plat_path(console, local, sizeof(local));
  if(fs_read_file(local, &data, &len, 256 * 1024)) {
    return;
  }
  if((pj = json_parse(data, len))) {
    lp = json_get(pj, "localizedParameters");
    t = json_get_str(json_get(lp, json_get_str(lp, "defaultLanguage", "en-US")), "titleName", NULL);
    if(!t) {
      t = json_get_str(json_get(lp, "en-US"), "titleName", NULL);
    }
    if(t) {
      snprintf(out, size, "%s", t);
    }
    json_free(pj);
  }
  free(data);
}

/* ------------------------------------------------------------ websrv */

static int
websrv_port(void) {
  const char *env = getenv("NEOWEB_WEBSRV_PORT"); /* the tests' stand-in */
  return env && atoi(env) > 0 ? atoi(env) : WEBSRV_PORT;
}

/* One GET to websrv on 127.0.0.1; the HTTP status, or -1 when it did not
 * answer. */
static int
websrv_get(const char *path_query, int timeout_ms) {
  struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  struct sockaddr_in addr;
  char buf[512];
  size_t got = 0;
  int fd, status = -1;
  char *head;

  if(!(head = malloc(strlen(path_query) + 128))) {
    return -1;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)websrv_port());
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    free(head);
    return -1;
  }
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
    close(fd);
    free(head);
    return -1;
  }
  sprintf(head, "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path_query);
  if(!http_send_all(fd, head, strlen(head))) {
    while(got + 1 < sizeof(buf)) {
      ssize_t n = recv(fd, buf + got, sizeof(buf) - 1 - got, 0);
      if(n < 0 && errno == EINTR) {
        continue;
      }
      if(n <= 0) {
        break;
      }
      got += (size_t)n;
      buf[got] = 0;
      if(strstr(buf, "\r\n")) {
        break;
      }
    }
    buf[got] = 0;
    if(sscanf(buf, "HTTP/%*s %d", &status) != 1) {
      status = -1;
    }
  }
  close(fd);
  free(head);
  return status;
}

static int
websrv_running(void) {
  return websrv_get("/version", 400) == 200;
}

/* percent-encode for a query string value */
static int
qenc(char *out, size_t size, size_t *at, const char *s) {
  static const char hex[] = "0123456789ABCDEF";
  for(; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
       c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
      if(*at + 1 >= size) {
        return -1;
      }
      out[(*at)++] = (char)c;
    } else {
      if(*at + 3 >= size) {
        return -1;
      }
      out[(*at)++] = '%';
      out[(*at)++] = hex[c >> 4];
      out[(*at)++] = hex[c & 15];
    }
  }
  out[*at] = 0;
  return 0;
}

/* websrv's own encoding (assets/apiClient.js launchApp): every space in a
 * value is "\ ", values are joined by one space. */
static int
add_word(char *out, size_t size, size_t *at, const char *s) {
  if(*at && *at + 1 < size) {
    out[(*at)++] = ' ';
  }
  for(; *s; s++) {
    if(*s == ' ') {
      if(*at + 2 >= size) {
        return -1;
      }
      out[(*at)++] = '\\';
    }
    if(*at + 1 >= size) {
      return -1;
    }
    out[(*at)++] = *s;
  }
  out[*at] = 0;
  return 0;
}

/* args: a string (taken as websrv takes it) or an array of strings */
static int
encode_args(const jval_t *v, char *out, size_t size) {
  size_t at = 0;
  out[0] = 0;
  if(!v || v->type == J_NULL) {
    return 0;
  }
  if(v->type == J_STR) {
    /* websrv escapes the spaces of a string too: it is one argument */
    return add_word(out, size, &at, v->str);
  }
  if(v->type != J_ARR) {
    return -1;
  }
  for(const jval_t *c = v->child; c; c = c->next) {
    if(c->type == J_NUM) {
      if(add_word(out, size, &at, c->str)) {
        return -1;
      }
    } else if(c->type != J_STR || add_word(out, size, &at, c->str)) {
      return -1;
    }
  }
  return 0;
}

/* env: an object {"NAME": "value"} */
static int
encode_env(const jval_t *v, char *out, size_t size) {
  size_t at = 0;
  out[0] = 0;
  if(!v || v->type == J_NULL) {
    return 0;
  }
  if(v->type != J_OBJ) {
    return -1;
  }
  for(const jval_t *c = v->child; c; c = c->next) {
    char kv[HB_ARGS_MAX];
    if(!c->key || !c->key[0] || strchr(c->key, '=') || (c->type != J_STR && c->type != J_NUM)) {
      return -1;
    }
    if(snprintf(kv, sizeof(kv), "%s=%s", c->key, c->str) >= (int)sizeof(kv) ||
       add_word(out, size, &at, kv)) {
      return -1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------ list */

typedef struct hb_app {
  char dir[PATH_MAX];
  char name[256];
  char title[256];
  int elf, js, icon;
} hb_app_t;

static int
app_cmp(const void *a, const void *b) {
  const hb_app_t *x = a, *y = b;
  int c = strcasecmp(x->name, y->name);
  return c ? c : strcmp(x->dir, y->dir);
}

/* GET /api/homebrew/list */
void
api_homebrew_list(http_req_t *req) {
  hb_app_t *apps = calloc(HB_MAX_APPS, sizeof(*apps));
  size_t n = 0;
  jw_t w;

  if(!apps) {
    http_send_error(req, 500, "out of memory");
    return;
  }
  api_begin(&w);
  jw_key(&w, "roots");
  jw_arr(&w);
  for(size_t i = 0; i < HB_NROOTS; i++) {
    char local[PATH_MAX];
    struct dirent *e;
    DIR *d;

    plat_path(HB_ROOTS[i], local, sizeof(local));
    if(!(d = opendir(local))) {
      continue;
    }
    jw_str(&w, HB_ROOTS[i]);
    while((e = readdir(d)) && n < HB_MAX_APPS) {
      char path[PATH_MAX];
      hb_app_t *a = &apps[n];

      if(!plain_name(e->d_name, strlen(e->d_name))) {
        continue;
      }
      snprintf(a->dir, sizeof(a->dir), "%s/%s", HB_ROOTS[i], e->d_name);
      snprintf(path, sizeof(path), "%s/eboot.elf", a->dir);
      a->elf = is_file(path);
      snprintf(path, sizeof(path), "%s/homebrew.js", a->dir);
      a->js = is_file(path);
      if(!a->elf && !a->js) {
        continue;
      }
      snprintf(a->name, sizeof(a->name), "%s", e->d_name);
      snprintf(path, sizeof(path), "%s/sce_sys/icon0.png", a->dir);
      a->icon = is_file(path);
      param_title(a->dir, a->title, sizeof(a->title));
      n++;
    }
    closedir(d);
  }
  jw_arr_end(&w);
  if(n) {
    qsort(apps, n, sizeof(*apps), app_cmp);
  }
  jw_key(&w, "apps");
  jw_arr(&w);
  for(size_t i = 0; i < n; i++) {
    const char *slash = strrchr(apps[i].dir, '/');
    jw_obj(&w);
    jw_kstr(&w, "dir", apps[i].dir);
    jw_key(&w, "root");
    jw_strn(&w, apps[i].dir, (size_t)(slash - apps[i].dir));
    jw_kstr(&w, "name", apps[i].name);
    jw_kstr(&w, "title", apps[i].title[0] ? apps[i].title : apps[i].name);
    jw_kbool(&w, "elf", apps[i].elf);
    jw_kbool(&w, "js", apps[i].js);
    jw_kbool(&w, "icon", apps[i].icon);
    jw_obj_end(&w);
  }
  jw_arr_end(&w);
#ifdef NEOWEB_FULL
  jw_kbool(&w, "launch", 1);
  jw_kbool(&w, "websrv", websrv_running());
#else
  jw_kbool(&w, "launch", 0);
  jw_kbool(&w, "websrv", 0);
#endif
  free(apps);
  api_finish(req, &w);
}

/* GET /api/homebrew/icon?dir= */
void
api_homebrew_icon(http_req_t *req) {
  char dir[PATH_MAX], console[PATH_MAX], local[PATH_MAX];

  if(query_dir(req, dir, sizeof(dir))) {
    return;
  }
  snprintf(console, sizeof(console), "%s/sce_sys/icon0.png", dir);
  plat_path(console, local, sizeof(local));
  http_send_file_ex(req, local, 0, "no-cache");
}

#ifdef NEOWEB_FULL

/* GET /api/homebrew/js?dir= */
void
api_homebrew_js(http_req_t *req) {
  char dir[PATH_MAX], console[PATH_MAX], local[PATH_MAX];
  char *data = NULL;
  size_t len = 0;
  jw_t w;

  if(query_dir(req, dir, sizeof(dir))) {
    return;
  }
  snprintf(console, sizeof(console), "%s/homebrew.js", dir);
  plat_path(console, local, sizeof(local));
  if(fs_read_file(local, &data, &len, HB_JS_MAX)) {
    http_send_error(req, 404, "%s has no homebrew.js", dir);
    return;
  }
  api_begin(&w);
  jw_kstr(&w, "dir", dir);
  jw_key(&w, "js");
  jw_strn(&w, data, len);
  free(data);
  api_finish(req, &w);
}

/* path is dir itself or inside it */
static int
inside(const char *dir, const char *path) {
  size_t n = strlen(dir);
  return clean_abs(path) && !strncmp(path, dir, n) && (!path[n] || path[n] == '/');
}

/* GET /api/homebrew/ls?dir=&path= : what websrv's ApiClient.fsListDir gives
 * a homebrew.js, for a folder inside the app's own folder only */
void
api_homebrew_ls(http_req_t *req) {
  char dir[PATH_MAX], path[PATH_MAX], local[PATH_MAX];
  struct dirent *e;
  DIR *d;
  jw_t w;

  if(query_dir(req, dir, sizeof(dir))) {
    return;
  }
  if(!http_query(req, "path", path, sizeof(path)) || !path[0]) {
    snprintf(path, sizeof(path), "%s", dir);
  }
  while(strlen(path) > 1 && path[strlen(path) - 1] == '/') {
    path[strlen(path) - 1] = 0;
  }
  if(!inside(dir, path)) {
    http_send_error(req, 403, "%s is outside %s", path, dir);
    return;
  }
  plat_path(path, local, sizeof(local));
  if(!(d = opendir(local))) {
    http_send_error(req, 404, "%s: %s", path, strerror(errno));
    return;
  }
  api_begin(&w);
  jw_kstr(&w, "path", path);
  jw_key(&w, "entries");
  jw_arr(&w);
  while((e = readdir(d))) {
    char child[PATH_MAX + 256];
    struct stat st;
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    snprintf(child, sizeof(child), "%s/%s", local, e->d_name);
    if(stat(child, &st)) {
      continue;
    }
    jw_obj(&w);
    jw_kstr(&w, "name", e->d_name);
    jw_kbool(&w, "dir", S_ISDIR(st.st_mode));
    jw_kuint(&w, "size", (uint64_t)st.st_size);
    jw_kint(&w, "mtime", (int64_t)st.st_mtime);
    jw_obj_end(&w);
  }
  closedir(d);
  jw_arr_end(&w);
  api_finish(req, &w);
}

static int
is_elf(const char *console) {
  char local[PATH_MAX];
  unsigned char magic[4];
  int fd, ok;

  plat_path(console, local, sizeof(local));
  if((fd = open(local, O_RDONLY)) < 0) {
    return 0;
  }
  ok = read(fd, magic, 4) == 4 && !memcmp(magic, "\x7f" "ELF", 4);
  close(fd);
  return ok;
}

/* POST /api/homebrew/launch */
void
api_homebrew_launch(http_req_t *req) {
  char dir[PATH_MAX], path[PATH_MAX], cwd[PATH_MAX], local[PATH_MAX];
  char args[HB_ARGS_MAX], env[HB_ARGS_MAX];
  const char *via;
  jval_t *b = api_body(req);
  int daemon;
  jw_t w;

  if(!b) {
    return;
  }
  snprintf(dir, sizeof(dir), "%s", json_get_str(b, "dir", ""));
  snprintf(path, sizeof(path), "%s", json_get_str(b, "path", ""));
  snprintf(cwd, sizeof(cwd), "%s", json_get_str(b, "cwd", ""));
  daemon = json_get_bool(b, "daemon", 0);
  if(!path[0]) {
    snprintf(path, sizeof(path), "%s/eboot.elf", dir);
  }
  if(hb_dir_ok(dir)) {
    http_send_error(req, 404, "%s is not a homebrew folder", dir);
  } else if(!inside(dir, path) || !strcmp(path, dir)) {
    http_send_error(req, 403, "%s is not inside %s", path, dir);
  } else if(!is_file(path) || !is_elf(path)) {
    http_send_error(req, 404, "%s is not an ELF file", path);
  } else if(cwd[0] && !inside(dir, cwd)) {
    http_send_error(req, 403, "the working directory must be inside %s", dir);
  } else if(encode_args(json_get(b, "args"), args, sizeof(args))) {
    http_send_error(req, 400, "'args' must be a string or a list of strings");
  } else if(encode_env(json_get(b, "env"), env, sizeof(env))) {
    http_send_error(req, 400, "'env' must be an object of strings");
  } else if(!args[0] && !env[0] && !cwd[0] && !daemon) {
    /* the same path as POST /api/payloads/start */
    plat_path(path, local, sizeof(local));
    if(plat_payload_launch(local)) {
      http_send_error(req, 502, "%s", plat_error());
    } else {
      LOG_INFO("homebrew started: %s from %s", path, req->client_ip);
      via = "elfldr";
      goto done;
    }
  } else if(!websrv_running()) {
    http_send_error(req, 409, "this launch passes arguments, which only websrv can do: "
                    "start websrv.elf (Payloads) and try again");
  } else {
    /* each value at most triples when encoded */
    size_t qsize = 3 * (sizeof(path) + sizeof(cwd) + sizeof(args) + sizeof(env)) + 128;
    char *q = malloc(qsize);
    size_t at;
    int st;

    if(!q) {
      http_send_error(req, 500, "out of memory");
      json_free(b);
      return;
    }
    at = (size_t)sprintf(q, "/hbldr?pipe=0&daemon=%d&path=", daemon ? 1 : 0);
    qenc(q, qsize, &at, path);
    if(args[0]) {
      at += (size_t)sprintf(q + at, "&args=");
      qenc(q, qsize, &at, args);
    }
    if(env[0]) {
      at += (size_t)sprintf(q + at, "&env=");
      qenc(q, qsize, &at, env);
    }
    if(cwd[0]) {
      at += (size_t)sprintf(q + at, "&cwd=");
      qenc(q, qsize, &at, cwd);
    }
    st = websrv_get(q, 15000);
    free(q);
    if(st != 200) {
      http_send_error(req, 502, st < 0 ? "websrv did not answer" : "websrv could not launch it (HTTP %d)", st);
    } else {
      LOG_INFO("homebrew started through websrv: %s %s from %s", path, args, req->client_ip);
      via = "websrv";
      goto done;
    }
  }
  json_free(b);
  return;

done:
  json_free(b);
  api_begin(&w);
  jw_kstr(&w, "path", path);
  jw_kstr(&w, "via", via);
  api_finish(req, &w);
}

#endif /* NEOWEB_FULL */
