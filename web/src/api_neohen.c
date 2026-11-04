/* NeoWEB - NeoHEN's own settings (its config.ini) and a backup of every
 * setting NeoHEN and NeoWEB keep.
 *
 * Settings. NeoWEB edits config.ini line by line: a known key gets its new
 * value where it is, a missing one is added at the end of its section, and
 * every other line (comments, keys NeoWEB does not know, the user's own
 * layout) stays as it was. Only the keys of g_keys can be changed, each
 * with the values NeoHEN itself accepts (libonion_settings/settings.cpp).
 *
 * Who reads a change, and when:
 *   now     the daemon reads config.ini again within a second of any
 *           change (it polls its mtime); util is told to read it again
 *           here (BREW_RELOAD_SETTINGS on its socket, as the Toolbox does)
 *   resume  the daemon has it at once, but it only matters on the next
 *           Rest Mode resume
 *   start   read once, when NeoHEN starts (bootstrapper, daemon start)
 *   toolbox the Toolbox (inside SceShellUI) keeps its own copy: it reads
 *           config.ini again when its settings page is opened and when
 *           NeoHEN starts. It writes its whole copy back when something is
 *           changed there, so a Toolbox settings page left open while
 *           NeoWEB changes things saves the old values over them.
 *
 * Backup. GET /api/neohen/backup is one JSON file with config.ini,
 * NeoWEB's config.json (the token is in it, as it is the user's own copy),
 * store.json, apps.json, ui.json (the page's looks) and games.json (notes).
 * POST /api/neohen/restore takes that file back: everything is checked
 * before anything is written; then each file present is copied to
 * <file>.bak and replaced through a temp file and a rename.
 *
 * Both are in the full build only: the restricted one does not change
 * NeoHEN's configuration (the Toolbox has the same settings).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "fsutil.h"
#include "log.h"
#include "plat.h"

#define NEOHEN_INI         "/data/neohen/config.ini"
/* the same file as SceShellUI's sandbox sees it; settings.cpp writes both */
#define NEOHEN_INI_SHELLUI "/user/data/neohen/config.ini"
#define UTIL_RELOAD        0xC0FFEEu /* BREW_RELOAD_SETTINGS */
#define INI_MAX            (1 << 20)

typedef struct nh_key {
  const char *key;     /* section.name */
  char        type;    /* 'e' one of choices, 'b' true/false, 'i' a number */
  const char *choices; /* 'e': "a|b|c" */
  int         min, max;
  const char *def;     /* NeoHEN's default, when the file does not say */
  const char *when;    /* now, resume, start, toolbox (see the top) */
} nh_key_t;

static const nh_key_t g_keys[] = {
  {"update.channel", 'e', "stable|prerelease", 0, 0, "stable", "now"},
  {"neoweb.watchdog", 'e', "off|notify|relaunch", 0, 0, "relaunch", "now"},
  {"shadowmount.watchdog", 'e', "off|notify|relaunch", 0, 0, "relaunch", "now"},
  {"shadowmount.watchdog_interval_seconds", 'i', NULL, 30, 3600, "60", "now"},
  {"rest_mode.toolbox_after_resume", 'e', "manual|stable|original", 0, 0, "stable", "resume"},
  {"rest_mode.stable_seconds", 'i', NULL, 10, 300, "30", "resume"},
  {"rest_mode.resume_reinject_delay_seconds", 'i', NULL, 0, 255, "10", "resume"},
  {"startup.open_after_load", 'e', "none|home_menu", 0, 0, "none", "start"},
  {"startup.show_thanks", 'b', NULL, 0, 0, "true", "start"},
  {"kstuff.autoload", 'b', NULL, 0, 0, "true", "start"},
  {"shellui.wait_for_tracer", 'b', NULL, 0, 0, "true", "now"},
  {"toolbox.language", 'e', "system|en|es|fr|de|it|pt-BR|ja|ko|zh-Hans|zh-Hant|ru|pl|ar|th", 0, 0, "system", "toolbox"},
  {"home_screen.show_title_ids", 'b', NULL, 0, 0, "false", "toolbox"},
  {"game_menu.show_onionhen_options", 'b', NULL, 0, 0, "true", "toolbox"},
  {"shortcuts.cheats_menu", 'e', "off|r3_l3|l2_triangle|long_options|long_share|share", 0, 0, "off", "toolbox"},
  {"shortcuts.toolbox", 'e', "off|l2_r3|long_share|share", 0, 0, "off", "toolbox"},
  {"cheats.mirror", 'e', "auto|github|cnb", 0, 0, "auto", "now"},
  {"logging.level", 'e', "off|error|warn|info|debug|trace", 0, 0, "info", "now"},
  {"logging.max_bytes", 'i', NULL, 65536, 67108864, "786432", "now"},
};
#define NKEYS (sizeof(g_keys) / sizeof(g_keys[0]))

/* ------------------------------------------------------------ the INI */

static const nh_key_t *
key_find(const char *key) {
  for(size_t i = 0; i < NKEYS; i++) {
    if(!strcmp(g_keys[i].key, key)) {
      return &g_keys[i];
    }
  }
  return NULL;
}

/* one line of the file, trimmed: [b, e) */
static void
trim(const char **b, const char **e) {
  while(*b < *e && isspace((unsigned char)**b)) {
    (*b)++;
  }
  while(*e > *b && isspace((unsigned char)(*e)[-1])) {
    (*e)--;
  }
}

/*
 * Walk the lines of `text` as NeoHEN's parser reads them (include/ini.h:
 * trimmed, ';' and '#' start a comment, [section], key=value). For `key`
 * (section.name) it gives where its value is (*vb, *ve) and, when the key
 * is not there, where the section's last setting line ends (*sec_end, or
 * NULL without the section). Returns 1 when the key is there.
 */
static int
ini_find(const char *text, const char *key, const char **vb, const char **ve,
         const char **sec_end) {
  const char *dot = strchr(key, '.');
  size_t slen = (size_t)(dot - key), klen = strlen(dot + 1);
  int in = 0;

  *sec_end = NULL;
  for(const char *line = text; *line;) {
    const char *nl = strchr(line, '\n');
    const char *end = nl ? nl : line + strlen(line);
    const char *lb = line, *le = end;
    trim(&lb, &le);
    if(lb < le && *lb == '[' && le[-1] == ']') {
      in = (size_t)(le - lb - 2) == slen && !strncasecmp(lb + 1, key, slen);
      if(in) {
        *sec_end = nl ? nl + 1 : end;
      }
    } else if(in && lb < le && *lb != ';' && *lb != '#') {
      const char *eq = memchr(lb, '=', (size_t)(le - lb));
      if(eq) {
        const char *kb = lb, *ke = eq;
        trim(&kb, &ke);
        *sec_end = nl ? nl + 1 : end;
        if((size_t)(ke - kb) == klen && !strncasecmp(kb, dot + 1, klen)) {
          *vb = eq + 1;
          *ve = le;
          trim(vb, ve);
          return 1;
        }
      }
    }
    line = nl ? nl + 1 : end;
  }
  return 0;
}

static void
ini_get(const char *text, const char *key, char *out, size_t size) {
  const char *vb, *ve, *se;
  out[0] = 0;
  if(ini_find(text, key, &vb, &ve, &se)) {
    snprintf(out, size, "%.*s", (int)(ve - vb), vb);
  }
}

/* a new copy of the text with key=value; the old one is freed */
static char *
ini_set(char *text, const char *key, const char *value) {
  const char *vb, *ve, *se, *dot = strchr(key, '.');
  size_t tlen = strlen(text), add = strlen(key) + strlen(value) + 8;
  char *out = malloc(tlen + add + 1), *o = out;

  if(!out) {
    free(text);
    return NULL;
  }
  if(ini_find(text, key, &vb, &ve, &se)) {
    o += sprintf(o, "%.*s%s%s", (int)(vb - text), text, value, ve);
  } else if(se) {
    /* at the end of its section, after its last setting */
    int nl = se > text && se[-1] != '\n';
    o += sprintf(o, "%.*s%s%s=%s\n%s", (int)(se - text), text, nl ? "\n" : "", dot + 1, value, se);
  } else {
    int nl = tlen && text[tlen - 1] != '\n';
    o += sprintf(o, "%s%s\n[%.*s]\n%s=%s\n", text, nl ? "\n" : "", (int)(dot - key), key, dot + 1, value);
  }
  free(text);
  return out;
}

/* NeoHEN ignores the whole file without this (settings.cpp apply_parser) */
static int
ini_valid(const char *text) {
  char v[16];
  ini_get(text, "meta.schema_version", v, sizeof(v));
  return !strcmp(v, "1");
}

/* Put `body` in place of `console`: a temp file next to it and a rename,
 * never half a file. With `bak`, the file there now is copied to .bak
 * first. The mtime always moves on: the daemon reloads when it is newer
 * than the one it last read, to the second. */
static int
replace_file(const char *console, const char *body, size_t len, int bak) {
  char local[PATH_MAX];
  struct stat old;
  int had;

  plat_path(console, local, sizeof(local));
  had = !stat(local, &old) && S_ISREG(old.st_mode);
  if(bak && had) {
    char b[PATH_MAX + 8];
    snprintf(b, sizeof(b), "%s.bak", local);
    if(fs_copy_file(local, b, NULL)) {
      return -1;
    }
  }
  /* fs_write_file writes <file>.tmp and renames it over the file */
  if(fs_write_file(local, body, len)) {
    return -1;
  }
  if(had) {
    struct stat now;
    /* the Toolbox (SceShellUI) writes this file too: same owner and mode */
    if(chown(local, old.st_uid, old.st_gid)) {
      /* best effort: not every process may give a file away */
    }
    chmod(local, old.st_mode & 07777);
    if(!stat(local, &now) && now.st_mtime <= old.st_mtime) {
      struct timeval tv[2] = {{old.st_mtime + 1, 0}, {old.st_mtime + 1, 0}};
      utimes(local, tv);
    }
  }
  return 0;
}

/* the twin as SceShellUI sees it, when that is another file NeoWEB can see */
static int
twin_present(void) {
  char a[PATH_MAX], b[PATH_MAX];
  struct stat sa, sb;
  plat_path(NEOHEN_INI, a, sizeof(a));
  plat_path(NEOHEN_INI_SHELLUI, b, sizeof(b));
  return !stat(b, &sb) && S_ISREG(sb.st_mode) &&
         (stat(a, &sa) || sa.st_ino != sb.st_ino || sa.st_dev != sb.st_dev);
}

/* util keeps its copy until told; the daemon looks at the mtime itself */
static int
util_reload(void) {
  char var[64];
  jw_t q;
  int rc;
  jw_init(&q);
  jw_obj(&q);
  jw_obj_end(&q);
  rc = neohen_util_call(UTIL_RELOAD, &q, var, sizeof(var), 5);
  jw_free(&q);
  return rc == 0;
}

/* ------------------------------------------------------------ settings */

static int
read_ini(http_req_t *req, char **text) {
  char local[PATH_MAX];
  size_t len = 0;
  plat_path(NEOHEN_INI, local, sizeof(local));
  if(fs_read_file(local, text, &len, INI_MAX)) {
    http_send_error(req, 404, "NeoHEN's config.ini is not there (NeoHEN writes it when it starts)");
    return -1;
  }
  return 0;
}

/* GET /api/neohen/settings
 * {"keys":[{"key","type","choices"|"min","max","default","value","set","when"}]} */
void
api_neohen_settings_get(http_req_t *req) {
  char *text = NULL, v[256];
  jw_t w;

  if(read_ini(req, &text)) {
    return;
  }
  api_begin(&w);
  jw_kstr(&w, "path", NEOHEN_INI);
  jw_kbool(&w, "valid", ini_valid(text));
  jw_key(&w, "keys");
  jw_arr(&w);
  for(size_t i = 0; i < NKEYS; i++) {
    const nh_key_t *k = &g_keys[i];
    ini_get(text, k->key, v, sizeof(v));
    jw_obj(&w);
    jw_kstr(&w, "key", k->key);
    jw_kstr(&w, "type", k->type == 'e' ? "enum" : k->type == 'b' ? "bool" : "int");
    if(k->type == 'e') {
      jw_key(&w, "choices");
      jw_arr(&w);
      for(const char *c = k->choices; *c; c += strcspn(c, "|"), c += *c == '|') {
        jw_strn(&w, c, strcspn(c, "|"));
      }
      jw_arr_end(&w);
    } else if(k->type == 'i') {
      jw_kint(&w, "min", k->min);
      jw_kint(&w, "max", k->max);
    }
    jw_kstr(&w, "default", k->def);
    jw_kstr(&w, "value", v[0] ? v : k->def);
    jw_kbool(&w, "set", v[0] != 0);
    jw_kstr(&w, "when", k->when);
    jw_obj_end(&w);
  }
  jw_arr_end(&w);
  free(text);
  api_finish(req, &w);
}

/* the value as config.ini spells it, or an error */
static const char *
key_value(const nh_key_t *k, const jval_t *v, char *out, size_t size) {
  if(k->type == 'b') {
    if(v->type == J_BOOL) {
      snprintf(out, size, "%s", v->boolean ? "true" : "false");
    } else if(v->type == J_STR && (!strcasecmp(v->str, "true") || !strcasecmp(v->str, "false"))) {
      snprintf(out, size, "%s", strcasecmp(v->str, "true") ? "false" : "true");
    } else {
      return "true or false";
    }
    return NULL;
  }
  if(k->type == 'i') {
    char *end;
    long n;
    if(v->type == J_NUM) {
      n = (long)v->num;
      if((double)n != v->num) {
        return "a whole number";
      }
    } else if(v->type == J_STR && v->str[0]) {
      n = strtol(v->str, &end, 10);
      if(*end) {
        return "a whole number";
      }
    } else {
      return "a whole number";
    }
    if(n < k->min || n > k->max) {
      return "out of range";
    }
    snprintf(out, size, "%ld", n);
    return NULL;
  }
  if(v->type != J_STR) {
    return "one of the listed values";
  }
  for(const char *c = k->choices; *c; c += strcspn(c, "|"), c += *c == '|') {
    size_t n = strcspn(c, "|");
    if(strlen(v->str) == n && !strncasecmp(c, v->str, n)) {
      snprintf(out, size, "%.*s", (int)n, c);
      return NULL;
    }
  }
  return "one of the listed values";
}

/* POST /api/neohen/settings {"values":{"update.channel":"prerelease",...}}
 * -> {"changed":[...],"when":{"now":n,...},"util":bool} */
void
api_neohen_settings_set(http_req_t *req) {
  char *text = NULL, val[64], cm[32], tb[32];
  jval_t *body = api_body(req), *vals, *v;
  int n = 0;
  jw_t w;

  if(!body) {
    return;
  }
  vals = json_get(body, "values");
  if(!vals || vals->type != J_OBJ || !vals->child) {
    json_free(body);
    http_send_error(req, 400, "missing 'values'");
    return;
  }
  for(v = vals->child; v; v = v->next) {
    const nh_key_t *k = key_find(v->key);
    const char *bad = k ? key_value(k, v, val, sizeof(val)) : "not a setting NeoWEB can change";
    if(bad) {
      http_send_error(req, 400, "%s: %s", v->key, bad);
      json_free(body);
      return;
    }
  }
  if(read_ini(req, &text)) {
    json_free(body);
    return;
  }
  if(!ini_valid(text)) {
    free(text);
    json_free(body);
    http_send_error(req, 409, "config.ini has no [meta] schema_version=1: NeoHEN ignores it as it is, so it is left alone");
    return;
  }
  for(v = vals->child; v && text; v = v->next) {
    key_value(key_find(v->key), v, val, sizeof(val));
    text = ini_set(text, v->key, val);
    n++;
  }
  if(!text) {
    json_free(body);
    http_send_error(req, 500, "out of memory");
    return;
  }
  /* the Toolbox refuses the same button for both shortcuts */
  ini_get(text, "shortcuts.cheats_menu", cm, sizeof(cm));
  ini_get(text, "shortcuts.toolbox", tb, sizeof(tb));
  if((!strcasecmp(cm, "share") && !strcasecmp(tb, "share")) ||
     (!strcasecmp(cm, "long_share") && !strcasecmp(tb, "long_share"))) {
    free(text);
    json_free(body);
    http_send_error(req, 409, "the cheats menu and the Toolbox cannot have the same shortcut");
    return;
  }
  if(replace_file(NEOHEN_INI, text, strlen(text), 0) ||
     (twin_present() && replace_file(NEOHEN_INI_SHELLUI, text, strlen(text), 0))) {
    free(text);
    json_free(body);
    http_send_error(req, 500, "config.ini: %s", strerror(errno));
    return;
  }
  free(text);
  LOG_INFO("NeoHEN settings: %d changed from %s", n, req->client_ip);
  api_begin(&w);
  jw_key(&w, "changed");
  jw_arr(&w);
  for(v = vals->child; v; v = v->next) {
    jw_obj(&w);
    jw_kstr(&w, "key", v->key);
    jw_kstr(&w, "when", key_find(v->key)->when);
    jw_obj_end(&w);
  }
  jw_arr_end(&w);
  jw_kbool(&w, "util", util_reload());
  json_free(body);
  api_finish(req, &w);
}

/* ------------------------------------------------------------ backup */

/* what a backup holds: its name in the file, where it lives */
static const char *const g_files[][2] = {
  {"config.ini", NEOHEN_INI},
  {"web/config.json", PS5AIO_DATA_DIR "/config.json"},
  {"web/store.json", PS5AIO_DATA_DIR "/store.json"},
  {"web/apps.json", PS5AIO_DATA_DIR "/apps.json"},
  {"web/ui.json", PS5AIO_DATA_DIR "/ui.json"},
  {"web/games.json", PS5AIO_DATA_DIR "/games.json"},
};
#define NFILES (sizeof(g_files) / sizeof(g_files[0]))
#define BACKUP_FILE_MAX (1 << 20)

static const char *
file_path(const char *name) {
  for(size_t i = 0; i < NFILES; i++) {
    if(!strcmp(g_files[i][0], name)) {
      return g_files[i][1];
    }
  }
  return NULL;
}

/* GET /api/neohen/backup: {"neohenBackup":1,"made","neoweb","neohen","files":{}} */
void
api_neohen_backup(http_req_t *req) {
  char local[PATH_MAX], name[64], extra[160], *text;
  time_t now = time(NULL);
  size_t len;
  struct tm tm;
  jw_t w;

  jw_init(&w);
  jw_obj(&w);
  jw_kint(&w, "neohenBackup", 1);
  jw_kint(&w, "made", (int64_t)now);
  jw_kstr(&w, "neoweb", PS5AIO_VERSION);
  plat_path("/data/neohen/version.txt", local, sizeof(local));
  text = NULL;
  if(!fs_read_file(local, &text, &len, 128)) {
    while(len && isspace((unsigned char)text[len - 1])) {
      text[--len] = 0;
    }
    jw_kstr(&w, "neohen", text);
    free(text);
  }
  jw_key(&w, "files");
  jw_obj(&w);
  for(size_t i = 0; i < NFILES; i++) {
    text = NULL;
    plat_path(g_files[i][1], local, sizeof(local));
    if(!fs_read_file(local, &text, &len, BACKUP_FILE_MAX)) {
      jw_key(&w, g_files[i][0]);
      jw_strn(&w, text, len);
      free(text);
    }
  }
  jw_obj_end(&w);
  jw_obj_end(&w);
  if(w.oom) {
    jw_free(&w);
    http_send_error(req, 500, "out of memory");
    return;
  }
  localtime_r(&now, &tm);
  strftime(name, sizeof(name), "NeoHEN-settings-%Y%m%d-%H%M%S.json", &tm);
  snprintf(extra, sizeof(extra), "Content-Disposition: attachment; filename=\"%s\"\r\nCache-Control: no-store\r\n", name);
  http_send_buf_ex(req, 200, "application/json", w.data, w.len, extra);
  jw_free(&w);
}

/* the backup's files that can go back, or why not */
static const char *
check_file(const char *name, const jval_t *v) {
  if(!file_path(name)) {
    return "not a file a backup holds";
  }
  if(v->type != J_STR || !v->str) {
    return "not text";
  }
  size_t len = strlen(v->str);
  if(len > BACKUP_FILE_MAX) {
    return "too big";
  }
  if(!strcmp(name, "config.ini")) {
    return ini_valid(v->str) ? NULL : "no [meta] schema_version=1 (NeoHEN would ignore it)";
  }
  jval_t *j = json_parse(v->str, len);
  int ok = j && (j->type == J_OBJ || j->type == J_ARR);
  if(ok && !strcmp(name, "web/config.json")) {
    const jval_t *t = json_get(j, "token"), *g = json_get(j, "guestToken");
    ok = j->type == J_OBJ && (!t || t->type == J_STR) && (!g || g->type == J_STR);
  }
  json_free(j);
  return ok ? NULL : "not valid JSON";
}

/* store.json is also written by util: the same lock as api_store.c */
static int
store_lock(char *lock, size_t cap) {
  char local[PATH_MAX];
  plat_path(PS5AIO_DATA_DIR "/store.json", local, sizeof(local));
  snprintf(lock, cap, "%s.lock", local);
  for(int i = 0; i < 50; i++) {
    struct stat st;
    int fd = open(lock, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if(fd >= 0) {
      close(fd);
      return 1;
    }
    if(!stat(lock, &st) && time(NULL) - st.st_mtime > 30) {
      unlink(lock);
      continue;
    }
    usleep(100 * 1000);
  }
  return 0;
}

/* POST /api/neohen/restore, the backup as the body
 * -> {"restored":[names],"reloaded":{"tokens","util"}} */
void
api_neohen_restore(http_req_t *req) {
  size_t len = 0;
  char *raw = http_read_body_all(req, 8 * BACKUP_FILE_MAX, &len);
  jval_t *b, *files, *f;
  int tokens = 0, ini = 0, util = 0;
  jw_t w;

  if(!raw) {
    http_send_error(req, 400, "no backup in the request (8 MiB at most)");
    return;
  }
  b = json_parse(raw, len);
  free(raw);
  if(!b || b->type != J_OBJ || json_get_int(b, "neohenBackup", 0) != 1) {
    json_free(b);
    http_send_error(req, 400, "that is not a NeoHEN settings backup");
    return;
  }
  files = json_get(b, "files");
  if(!files || files->type != J_OBJ || !files->child) {
    json_free(b);
    http_send_error(req, 400, "the backup has no files");
    return;
  }
  for(f = files->child; f; f = f->next) {
    const char *bad = check_file(f->key, f);
    if(bad) {
      http_send_error(req, 400, "%s: %s; nothing was changed", f->key, bad);
      json_free(b);
      return;
    }
  }
  api_begin(&w);
  jw_key(&w, "restored");
  jw_arr(&w);
  for(f = files->child; f; f = f->next) {
    const char *console = file_path(f->key);
    char lock[PATH_MAX + 8];
    int held = 0, rc;
    if(!strcmp(f->key, "web/store.json")) {
      held = store_lock(lock, sizeof(lock));
    }
    rc = replace_file(console, f->str, strlen(f->str), 1);
    if(!rc && !strcmp(f->key, "config.ini") && twin_present()) {
      rc = replace_file(NEOHEN_INI_SHELLUI, f->str, strlen(f->str), 1);
    }
    if(held) {
      unlink(lock);
    }
    if(rc) {
      int e = errno;
      jw_arr_end(&w);
      jw_free(&w);
      http_send_error(req, 500, "%s: %s (the files before it are restored, their old copies are in .bak)",
                      f->key, strerror(e));
      json_free(b);
      return;
    }
    jw_str(&w, f->key);
    tokens |= !strcmp(f->key, "web/config.json");
    ini |= !strcmp(f->key, "config.ini");
  }
  jw_arr_end(&w);
  json_free(b);
  if(tokens) {
    api_tokens_reload();
  }
  if(ini) {
    util = util_reload();
  }
  LOG_INFO("settings restored from a backup by %s", req->client_ip);
  jw_kbool(&w, "util", util);
  api_finish(req, &w);
}
