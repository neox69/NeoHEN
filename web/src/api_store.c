/* NeoWEB - the payload store.
 *
 * The store: the page knows which payloads there are and asks GitHub what
 * their latest release is, but a browser is not allowed to download the
 * files. The console is: NeoHEN's util service fetches one release file
 * from github.com into the payloads folder when told to (see
 * BREW_UTIL_DOWNLOAD_FILE). What was installed, and which release, is kept
 * in /data/neohen/web/store.json so the page can tell what is out of date.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fsutil.h"
#include "log.h"
#include "plat.h"

#define STORE      PS5AIO_DATA_DIR "/store.json"
#define CMD_DOWNLOAD 0x800001Au /* BREW_UTIL_DOWNLOAD_FILE */

static jval_t *
store_load(void) {
  char local[PATH_MAX];
  char *data = NULL;
  jval_t *v = NULL;
  size_t len = 0;

  plat_path(STORE, local, sizeof(local));
  if(!fs_read_file(local, &data, &len, 1 << 20)) {
    v = json_parse(data, len);
    free(data);
  }
  if(!v || v->type != J_OBJ) {
    json_free(v);
    v = json_parse("{}", 2);
  }
  return v;
}

/* GET /api/store   what the store has installed: {"installed": {file: release}} */
void
api_store(http_req_t *req) {
  jval_t *s = store_load();
  jw_t w;

  api_begin(&w);
  jw_key(&w, "installed");
  if(json_get(s, "installed")) {
    json_write(&w, json_get(s, "installed"));
  } else {
    jw_obj(&w);
    jw_obj_end(&w);
  }
  /* what NeoHEN updated by itself: [{"name", "from", "to", "time"}] */
  jw_key(&w, "updates");
  if(json_get(s, "updates")) {
    json_write(&w, json_get(s, "updates"));
  } else {
    jw_arr(&w);
    jw_arr_end(&w);
  }
  json_free(s);
  api_finish(req, &w);
}

static int
payload_name_ok(const char *n) {
  size_t len = strlen(n);

  if(len < 5 || len > 60 || n[0] == '.' ||
     (strcmp(n + len - 4, ".elf") && strcmp(n + len - 4, ".bin"))) {
    return 0;
  }
  for(; *n; n++) {
    if(!isalnum((unsigned char)*n) && *n != '.' && *n != '_' && *n != '-') {
      return 0;
    }
  }
  return 1;
}

/* POST /api/store/fetch {"url": "https://github.com/...", "name": "x.elf", "tag": "v1.2"}
 * has the console download a release file into the payloads folder */
void
api_store_fetch(http_req_t *req) {
  char var[256], local[PATH_MAX];
  const char *url, *name, *tag;
  jval_t *b, *s;
  jw_t w, q;
  int rc;

  if(!(b = api_body(req))) {
    return;
  }
  url = json_get_str(b, "url", "");
  name = json_get_str(b, "name", "");
  tag = json_get_str(b, "tag", "");
  if(strncmp(url, "https://github.com/", 19) || !payload_name_ok(name) || strlen(tag) > 60) {
    json_free(b);
    http_send_error(req, 400, "a github.com release file and a .elf or .bin name are needed");
    return;
  }
  jw_init(&q);
  jw_obj(&q);
  jw_kstr(&q, "url", url);
  jw_kstr(&q, "name", name);
  jw_obj_end(&q);
  rc = neohen_util_call(CMD_DOWNLOAD, &q, var, sizeof(var), 150);
  jw_free(&q);
  LOG_INFO("store: %s <- %s: %s", name, url, rc < 0 ? "no answer" : var);
  if(rc < 0) {
    http_send_error(req, 502, "NeoHEN's service is not answering (this needs NeoHEN 0.1.0-rc5 or later)");
  } else if(rc) {
    http_send_error(req, 502, "the console could not download it: %s",
                    var[0] && strcmp(var, "Nothing") ? var : "unknown command");
  } else {
    s = store_load();
    json_set_str(json_get_or_add_obj(s, "installed"), name, tag);
    /* where it came from, so NeoHEN can look for newer releases of it */
    json_set_str(json_get_or_add_obj(s, "sources"), name, url);
    jw_init(&w);
    json_write(&w, s);
    plat_path(STORE, local, sizeof(local));
    fs_write_file(local, w.data, w.len);
    jw_free(&w);
    json_free(s);
    api_begin(&w);
    jw_kstr(&w, "name", name);
    jw_kint(&w, "bytes", atoll(var));
    api_finish(req, &w);
  }
  json_free(b);
}

/* GET/POST /api/store/auto {"enabled": bool}
 * whether NeoHEN keeps the payloads installed from the store up to date
 * (store_auto_off in NeoHEN's data folder turns it off) */
void
api_store_auto(http_req_t *req) {
  char local[PATH_MAX];
  struct stat st;
  jw_t w;

  plat_path("/data/neohen/store_auto_off", local, sizeof(local));
  if(!strcmp(req->method, "POST")) {
    jval_t *b = api_body(req);
    if(!b) {
      return;
    }
    int on = json_get_bool(b, "enabled", 1);
    json_free(b);
    if(on) {
      unlink(local);
    } else if(fs_write_file(local, "", 0)) {
      http_send_error(req, 500, "the setting could not be written");
      return;
    }
  }
  api_begin(&w);
  jw_kbool(&w, "enabled", stat(local, &st) != 0);
  api_finish(req, &w);
}
