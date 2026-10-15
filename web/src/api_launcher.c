/* NeoWEB - an icon on the PS5 home screen, in Media, that opens this page.
 *
 * It is a web based media app (applicationCategoryType 66048), the same kind
 * tv4play and plutotv install: an empty eboot.bin and a param.json whose
 * webAppUri the console opens in its own browser, full screen. The files go
 * to /system_ex/app/<id> and /user/app/<id>, and the console registers the
 * title from there. The address it opens can be changed, since which forms
 * the console accepts for webAppUri still has to be tried out.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

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

#ifndef LAUNCHER_DIR
#error "LAUNCHER_DIR must point to web/launcher"
#endif

#define LAUNCHER_ID  "NEOW00001"
#define LAUNCHER_URI "http://127.0.0.1:8686/"

__asm__(".section .rodata\n"
        ".global neoweb_icon0\n"
        "neoweb_icon0:\n"
        ".incbin \"" LAUNCHER_DIR "/icon0.png\"\n"
        ".global neoweb_icon0_end\n"
        "neoweb_icon0_end:\n"
        ".global neoweb_pic1\n"
        "neoweb_pic1:\n"
        ".incbin \"" LAUNCHER_DIR "/pic1.png\"\n"
        ".global neoweb_pic1_end\n"
        "neoweb_pic1_end:\n"
        ".previous\n");

extern const char neoweb_icon0[], neoweb_icon0_end[];
extern const char neoweb_pic1[], neoweb_pic1_end[];

static int
put(const char *console, const void *data, size_t len) {
  char local[PATH_MAX];
  plat_path(console, local, sizeof(local));
  return fs_write_file(local, data, len);
}

static int
mkdirs(const char *console) {
  char local[PATH_MAX];
  plat_path(console, local, sizeof(local));
  return fs_mkdirs(local, 0755);
}

/* The address the installed icon opens, or "" when there is none. */
static void
installed_uri(char *out, size_t size) {
  char local[PATH_MAX], *text = NULL;
  size_t len = 0;
  jval_t *v;

  out[0] = 0;
  plat_path("/user/app/" LAUNCHER_ID "/sce_sys/param.json", local, sizeof(local));
  if(fs_read_file(local, &text, &len, 64 * 1024)) {
    return;
  }
  if((v = json_parse(text, len))) {
    snprintf(out, size, "%s", json_get_str(v, "webAppUri", ""));
    json_free(v);
  }
  free(text);
}

/* GET /api/launcher */
void
api_launcher(http_req_t *req) {
  char uri[512], local[PATH_MAX];
  struct stat st;
  jw_t w;

  installed_uri(uri, sizeof(uri));
  plat_path("/user/appmeta/" LAUNCHER_ID, local, sizeof(local));
  api_begin(&w);
  jw_kstr(&w, "titleId", LAUNCHER_ID);
  jw_kbool(&w, "installed", uri[0] != 0);
  jw_kbool(&w, "registered", !stat(local, &st));
  jw_kstr(&w, "uri", uri);
  jw_kstr(&w, "defaultUri", LAUNCHER_URI);
  api_finish(req, &w);
}

/* POST /api/launcher/install {"uri": "http://127.0.0.1:8686/"} */
void
api_launcher_install(http_req_t *req) {
  char uri[512];
  jval_t *b = api_body(req);
  jw_t p;
  int bad;

  if(!b) {
    return;
  }
  snprintf(uri, sizeof(uri), "%s", json_get_str(b, "uri", LAUNCHER_URI));
  json_free(b);
  if(strncmp(uri, "http://", 7) && strncmp(uri, "https://", 8)) {
    http_send_error(req, 400, "the address must start with http:// or https://");
    return;
  }

  jw_init(&p);
  jw_obj(&p);
  jw_kint(&p, "applicationCategoryType", 66048);
  jw_kstr(&p, "titleId", LAUNCHER_ID);
  jw_key(&p, "localizedParameters");
  jw_obj(&p);
  jw_kstr(&p, "defaultLanguage", "en-US");
  jw_key(&p, "en-US");
  jw_obj(&p);
  jw_kstr(&p, "titleName", "NeoWEB");
  jw_obj_end(&p);
  jw_key(&p, "es-ES");
  jw_obj(&p);
  jw_kstr(&p, "titleName", "NeoWEB");
  jw_obj_end(&p);
  jw_obj_end(&p);
  jw_kint(&p, "downloadDataSize", 256);
  jw_kstr(&p, "webAppUri", uri);
  jw_obj_end(&p);
  if(p.oom) {
    jw_free(&p);
    http_send_error(req, 500, "out of memory");
    return;
  }

  /* a reinstall starts clean; a title that is not there is not an error */
  (void)plat_app_uninstall(LAUNCHER_ID);
  if(plat_remount_system_ex()) {
    LOG_WARN("launcher: %s", plat_error());
  }
  bad = mkdirs("/system_ex/app/" LAUNCHER_ID "/sce_sys") ||
        put("/system_ex/app/" LAUNCHER_ID "/eboot.bin", "", 0) ||
        put("/system_ex/app/" LAUNCHER_ID "/sce_sys/param.json", p.data, p.len) ||
        mkdirs("/user/app/" LAUNCHER_ID "/sce_sys") ||
        put("/user/app/" LAUNCHER_ID "/sce_sys/param.json", p.data, p.len) ||
        put("/user/app/" LAUNCHER_ID "/sce_sys/icon0.png", neoweb_icon0,
            (size_t)(neoweb_icon0_end - neoweb_icon0)) ||
        put("/user/app/" LAUNCHER_ID "/sce_sys/pic1.png", neoweb_pic1,
            (size_t)(neoweb_pic1_end - neoweb_pic1));
  jw_free(&p);
  if(bad) {
    http_send_error(req, 500, "the files of the icon could not be written: %s",
                    strerror(errno));
    return;
  }
  if(plat_app_install_dir(LAUNCHER_ID, "/user/app/")) {
    http_send_error(req, 500, "%s", plat_error());
    return;
  }
  LOG_INFO("launcher: %s installed, opens %s", LAUNCHER_ID, uri);
  http_send_ok(req);
}

/* GET /api/remoteplay
 * the account ID and the PIN the Toolbox showed last (Remote Play page), so
 * they can be pasted into Chiaki or PXPlay on the PC */
void
api_remoteplay(http_req_t *req) {
  char local[PATH_MAX], *text = NULL;
  size_t len = 0;
  jval_t *v = NULL;
  jw_t w;

  plat_path(PS5AIO_DATA_DIR "/remoteplay.json", local, sizeof(local));
  api_begin(&w);
  if(!fs_read_file(local, &text, &len, 4096) && (v = json_parse(text, len))) {
    jw_kbool(&w, "known", 1);
    jw_kstr(&w, "accountId", json_get_str(v, "accountId", ""));
    jw_kstr(&w, "accountIdDecimal", json_get_str(v, "accountIdDecimal", ""));
    jw_kstr(&w, "pin", json_get_str(v, "pin", ""));
    jw_kint(&w, "time", json_get_int(v, "time", 0));
    json_free(v);
  } else {
    jw_kbool(&w, "known", 0);
  }
  free(text);
  api_finish(req, &w);
}

/* POST /api/launcher/remove */
void
api_launcher_remove(http_req_t *req) {
  char local[PATH_MAX];

  http_drain_body(req);
  (void)plat_app_uninstall(LAUNCHER_ID);
  plat_path("/user/app/" LAUNCHER_ID, local, sizeof(local));
  fs_remove_tree(local, NULL);
  if(!plat_remount_system_ex()) {
    plat_path("/system_ex/app/" LAUNCHER_ID, local, sizeof(local));
    fs_remove_tree(local, NULL);
  }
  LOG_INFO("launcher: %s removed", LAUNCHER_ID);
  http_send_ok(req);
}
