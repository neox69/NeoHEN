/* NeoWEB - NeoHEN's own update (System > NeoHEN update).
 *
 * NeoHEN's util service does the work (BREW_UTIL_NEOHEN_UPDATE, see
 * source/util/source/neohen_update.cpp): it asks GitHub for the release
 * update.channel picks, downloads its zip, checks GitHub's SHA-256 and what
 * is inside, and keeps it in /data/neohen/update/current with the release
 * before it in previous/. The next NeoHEN.elf that starts hands off to it
 * (or "apply" restarts NeoHEN with it now). A release that does not reach
 * boot OK twice is rolled back by the bootstrapper. This file only relays:
 *
 * GET  /api/update   {"running", "channel", "installed": {state.json +
 *                    "current"/"previous" folders}, "available":
 *                    {available.json} | null, "job": {...} | null}
 * POST /api/update   {"action": "check" | "install" (+ "tag") | "cancel" |
 *                    "rollback" | "apply"}; answers like GET
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fsutil.h"
#include "log.h"
#include "plat.h"

#define UPDATE_DIR  "/data/neohen/update"
#define CONFIG_INI  "/data/neohen/config.ini"
#define VERSION_TXT "/data/neohen/version.txt"
#define CMD_UPDATE  0x8000030u /* BREW_UTIL_NEOHEN_UPDATE */

/* a small file as a JSON value, or null */
static void
put_json_file(jw_t *w, const char *path) {
  char local[PATH_MAX];
  char *data = NULL;
  size_t len = 0;
  jval_t *v = NULL;

  plat_path(path, local, sizeof(local));
  if(!fs_read_file(local, &data, &len, 1 << 20)) {
    v = json_parse(data, len);
    free(data);
  }
  if(v && v->type == J_OBJ) {
    json_write(w, v);
  } else {
    jw_null(w);
  }
  json_free(v);
}

/* first line of a small text file, only version characters */
static void
read_word(const char *path, char *out, size_t size) {
  char local[PATH_MAX];
  char *data = NULL;
  size_t len = 0, n = 0, i;

  out[0] = 0;
  plat_path(path, local, sizeof(local));
  if(fs_read_file(local, &data, &len, 4096)) {
    return;
  }
  for(i = 0; i < len && n + 1 < size; i++) {
    char c = data[i];
    if(c == '\n' || c == '\r') {
      break;
    }
    if(isalnum((unsigned char)c) || c == '.' || c == '-' || c == '+' || c == '_') {
      out[n++] = c;
    }
  }
  out[n] = 0;
  free(data);
}

/* update.channel in config.ini: stable (the default) or prerelease */
static const char *
channel(void) {
  char local[PATH_MAX], line[256];
  int in_update = 0, pre = 0;
  FILE *f;

  plat_path(CONFIG_INI, local, sizeof(local));
  if(!(f = fopen(local, "r"))) {
    return "stable";
  }
  while(fgets(line, sizeof(line), f)) {
    char *p = line, *e;
    while(isspace((unsigned char)*p)) {
      p++;
    }
    if(*p == '[') {
      in_update = !strncmp(p, "[update]", 8);
      continue;
    }
    if(!in_update || strncmp(p, "channel", 7)) {
      continue;
    }
    p += 7;
    while(*p == ' ' || *p == '\t') {
      p++;
    }
    if(*p++ != '=') {
      continue;
    }
    while(*p == ' ' || *p == '\t') {
      p++;
    }
    for(e = p; *e && !isspace((unsigned char)*e) && *e != ';' && *e != '#'; e++) {
    }
    pre = (size_t)(e - p) == 10 && !strncmp(p, "prerelease", 10);
  }
  fclose(f);
  return pre ? "prerelease" : "stable";
}

/* v0.1.0-rc16, 1.0.0: what util accepts as a tag */
static int
tag_ok(const char *t) {
  size_t n = strlen(t), i = t[0] == 'v';
  int dots = 0;

  if(!n || n > 40 || !isdigit((unsigned char)t[i])) {
    return 0;
  }
  for(; i < n; i++) {
    if(t[i] == '.') {
      dots++;
    } else if(!isalnum((unsigned char)t[i]) && t[i] != '-' && t[i] != '+') {
      return 0;
    }
  }
  return dots >= 2;
}

/* 0.1.0-rc16 -> 1000016... ; 0 when it is not a release number */
static long long
ver_key(const char *v) {
  long long k = 0;
  int i;

  if(*v == 'v') {
    v++;
  }
  for(i = 0; i < 3; i++) {
    char *e;
    long n = strtol(v, &e, 10);
    if(e == v || n < 0 || n > 9999 || (i < 2 && *e != '.')) {
      return 0;
    }
    k = k * 10000 + n;
    v = e + (i < 2);
  }
  /* a final release ranks above its rcs, other pre-releases below them */
  return k * 1000000 + (!strncmp(v, "-rc", 3) ? atol(v + 3) % 1000000 : *v == '-' ? 0 : 999999);
}

/* util knows the command from 0.1.0-rc16 on; an older one would put
 * "unknown command" on the TV each time. A developer build is asked. */
static int
util_knows(void) {
  char run[96];
  long long k;

  read_word(VERSION_TXT, run, sizeof(run));
  k = ver_key(run);
  return !k || k >= ver_key("0.1.0-rc16");
}

static void
reply(http_req_t *req, const char *job) {
  char cur[64], prev[64], run[96];
  jval_t *j = job ? json_parse(job, strlen(job)) : NULL;
  jw_t w;

  read_word(VERSION_TXT, run, sizeof(run));
  read_word(UPDATE_DIR "/current/version.txt", cur, sizeof(cur));
  read_word(UPDATE_DIR "/previous/version.txt", prev, sizeof(prev));
  api_begin(&w);
  jw_kstr(&w, "running", run);
  jw_kstr(&w, "channel", channel());
  jw_key(&w, "installed");
  jw_obj(&w);
  jw_kstr(&w, "current", cur);
  jw_kstr(&w, "previous", prev);
  jw_key(&w, "state");
  put_json_file(&w, UPDATE_DIR "/state.json");
  jw_obj_end(&w);
  jw_key(&w, "available");
  put_json_file(&w, UPDATE_DIR "/available.json");
  jw_key(&w, "job");
  if(j && j->type == J_OBJ) {
    json_write(&w, j);
  } else {
    jw_null(&w);
  }
  json_free(j);
  api_finish(req, &w);
}

void
api_update(http_req_t *req) {
  char var[1024];
  const char *action = "status";
  jval_t *b = NULL;
  jw_t q;
  int rc;

  jw_init(&q);
  jw_obj(&q);
  if(!strcmp(req->method, "POST")) {
    if(!(b = api_body(req))) {
      jw_free(&q);
      return;
    }
    action = json_get_str(b, "action", "");
    if(strcmp(action, "check") && strcmp(action, "install") && strcmp(action, "cancel") &&
       strcmp(action, "rollback") && strcmp(action, "apply")) {
      json_free(b);
      jw_free(&q);
      http_send_error(req, 400, "action is check, install, cancel, rollback or apply");
      return;
    }
    if(!strcmp(action, "install")) {
      const char *tag = json_get_str(b, "tag", "");
      if(!tag_ok(tag)) {
        json_free(b);
        jw_free(&q);
        http_send_error(req, 400, "a release tag (e.g. v0.1.0-rc16) is needed");
        return;
      }
      jw_kstr(&q, "tag", tag);
      LOG_INFO("update: install NeoHEN %s asked", tag);
    } else if(strcmp(action, "cancel")) {
      LOG_INFO("update: %s asked", action);
    }
  }
  jw_kstr(&q, "action", action);
  jw_obj_end(&q);
  if(!util_knows()) {
    jw_free(&q);
    json_free(b);
    if(strcmp(req->method, "POST")) {
      reply(req, NULL);
    } else {
      http_send_error(req, 409, "the loaded NeoHEN is older than this feature (it needs 0.1.0-rc16 or later): update it by hand once");
    }
    return;
  }
  rc = neohen_util_call(CMD_UPDATE, &q, var, sizeof(var), b ? 10 : 3);
  jw_free(&q);
  json_free(b);
  if(rc && strcmp(req->method, "POST")) {
    reply(req, NULL); /* the files still say what is installed */
    return;
  }
  if(rc < 0) {
    http_send_error(req, 502, "NeoHEN's service is not answering");
    return;
  }
  if(rc) {
    http_send_error(req, 409, "%s", var[0] && strcmp(var, "Nothing") ? var : "the console said no");
    return;
  }
  reply(req, var);
}
