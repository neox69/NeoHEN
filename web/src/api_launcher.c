/* NeoWEB - an icon on the PS5 home screen, in Media, that opens this page.
 *
 * It is a web shortcut (applicationCategoryType 65536 with a deeplinkUri), the
 * form Orbit Store, the WebKit Autoloader and Relapse use and that the console
 * is known to accept: a param.json and the icons in /user/app/<id>/sce_sys,
 * registered from there; the console stores deeplinkUri as the title's link
 * and opens that address in its browser. The first try, a web based media app
 * (66048 with webAppUri), registered but got psgm:play as its link, which runs
 * the title instead of opening the page.
 *
 * NeoWEB puts the icon there itself when it starts, unless it was removed from
 * the System page (the file launcher_off in its data folder).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fsutil.h"
#include "log.h"
#include "plat.h"

#ifndef LAUNCHER_DIR
#error "LAUNCHER_DIR must point to web/launcher"
#endif

#define LAUNCHER_ID  "NEOW00001"
#define LAUNCHER_URI "http://127.0.0.1:8686/"
#define LAUNCHER_OFF PS5AIO_DATA_DIR "/launcher_off"

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
    snprintf(out, size, "%s", json_get_str(v, "deeplinkUri", ""));
    json_free(v);
  }
  free(text);
}

/* The 66048 media app the first versions put there also left an empty
 * eboot.bin in /system_ex/app; it is no longer needed. */
static void
remove_old_system_ex(void) {
  char local[PATH_MAX];
  struct stat st;

  plat_path("/system_ex/app/" LAUNCHER_ID, local, sizeof(local));
  if(!stat(local, &st) && !plat_remount_system_ex()) {
    fs_remove_tree(local, NULL);
  }
}

/* Writes the icon's files and registers the title; 0, or -1 with a message. */
static int
launcher_install(const char *uri, char *err, size_t errsize) {
  jw_t p;
  int bad;

  jw_init(&p);
  jw_obj(&p);
  jw_kint(&p, "applicationCategoryType", 65536);
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
  jw_kstr(&p, "deeplinkUri", uri);
  jw_obj_end(&p);
  if(p.oom) {
    jw_free(&p);
    snprintf(err, errsize, "out of memory");
    return -1;
  }

  remove_old_system_ex();
  bad = mkdirs("/user/app/" LAUNCHER_ID "/sce_sys") ||
        put("/user/app/" LAUNCHER_ID "/sce_sys/param.json", p.data, p.len) ||
        put("/user/app/" LAUNCHER_ID "/sce_sys/icon0.png", neoweb_icon0,
            (size_t)(neoweb_icon0_end - neoweb_icon0)) ||
        put("/user/app/" LAUNCHER_ID "/sce_sys/pic1.png", neoweb_pic1,
            (size_t)(neoweb_pic1_end - neoweb_pic1));
  jw_free(&p);
  if(bad) {
    snprintf(err, errsize, "the files of the icon could not be written: %s", strerror(errno));
    return -1;
  }
  /* registering again over an existing title updates it in place */
  if(plat_app_install_dir(LAUNCHER_ID, "/user/app/")) {
    snprintf(err, errsize, "%s", plat_error());
    return -1;
  }
  LOG_INFO("launcher: %s installed, opens %s", LAUNCHER_ID, uri);
  return 0;
}

/* At start: put the icon there, or bring an old 66048 one to the new form,
 * unless it was removed on purpose. */
void
api_launcher_init(void) {
  char uri[512], local[PATH_MAX], err[256];
  struct stat st;

  plat_path(LAUNCHER_OFF, local, sizeof(local));
  if(!stat(local, &st)) {
    return;
  }
  installed_uri(uri, sizeof(uri));
  if(uri[0]) {
    return;
  }
  if(launcher_install(LAUNCHER_URI, err, sizeof(err))) {
    LOG_WARN("launcher: %s", err);
  }
}

/* GET /api/launcher */
void
api_launcher(http_req_t *req) {
  char uri[512];
  jw_t w;

  installed_uri(uri, sizeof(uri));
  api_begin(&w);
  jw_kstr(&w, "titleId", LAUNCHER_ID);
  jw_kbool(&w, "installed", uri[0] != 0);
  jw_kstr(&w, "uri", uri);
  jw_kstr(&w, "defaultUri", LAUNCHER_URI);
  api_finish(req, &w);
}

/* POST /api/launcher/install {"uri": "http://127.0.0.1:8686/"} */
void
api_launcher_install(http_req_t *req) {
  char uri[512], local[PATH_MAX], err[256];
  jval_t *b = api_body(req);

  if(!b) {
    return;
  }
  snprintf(uri, sizeof(uri), "%s", json_get_str(b, "uri", LAUNCHER_URI));
  json_free(b);
  if(strncmp(uri, "http://", 7) && strncmp(uri, "https://", 8)) {
    http_send_error(req, 400, "the address must start with http:// or https://");
    return;
  }
  if(launcher_install(uri, err, sizeof(err))) {
    http_send_error(req, 500, "%s", err);
    return;
  }
  plat_path(LAUNCHER_OFF, local, sizeof(local));
  unlink(local);
  http_send_ok(req);
}

/* GET /api/remoteplay
 * what the Toolbox last wrote: every user with its account ID (base64, as
 * Chiaki and PXPlay want it, and decimal), the pairing PIN and its state */
void
api_remoteplay(http_req_t *req) {
  char local[PATH_MAX], *text = NULL;
  size_t len = 0;
  jval_t *v = NULL;
  jw_t w;

  plat_path(PS5AIO_DATA_DIR "/remoteplay.json", local, sizeof(local));
  api_begin(&w);
  if(!fs_read_file(local, &text, &len, 64 * 1024) && (v = json_parse(text, len))) {
    jw_kbool(&w, "known", 1);
    jw_kint(&w, "now", (int64_t)time(NULL)); /* the console's clock, for the PIN countdown */
    jw_key(&w, "status");
    json_write(&w, v);
    json_free(v);
  } else {
    jw_kbool(&w, "known", 0);
  }
  free(text);
  api_finish(req, &w);
}

/* POST /api/remoteplay {"op": "list" | "pin" | "cancel" | "activate", "slot": 1}
 * left for the Toolbox, which carries it out within a second or two */
void
api_remoteplay_request(http_req_t *req) {
  char local[PATH_MAX], line[96];
  jval_t *b = api_body(req);
  const char *op;
  int slot;

  if(!b) {
    return;
  }
  op = json_get_str(b, "op", "list");
  slot = (int)json_get_int(b, "slot", 0);
  if(strcmp(op, "list") && strcmp(op, "pin") && strcmp(op, "cancel") && strcmp(op, "activate")) {
    json_free(b);
    http_send_error(req, 400, "op is list, pin, cancel or activate");
    return;
  }
  if(!strcmp(op, "activate") && (slot < 1 || slot > 16)) {
    json_free(b);
    http_send_error(req, 400, "slot goes from 1 to 16");
    return;
  }
  snprintf(line, sizeof(line), "{\"op\":\"%s\",\"slot\":%d}\n", op, slot);
  json_free(b);
  plat_path(PS5AIO_DATA_DIR "/rp_request.json", local, sizeof(local));
  if(fs_write_file(local, line, strlen(line))) {
    http_send_error(req, 500, "the request could not be written");
    return;
  }
  http_send_ok(req);
}

/* GET/POST /api/update/notice {"enabled": bool}
 * whether NeoHEN's util service says on the TV that a newer release is out;
 * the file update_check_off in NeoHEN's data folder turns it off */
void
api_update_notice(http_req_t *req) {
  char local[PATH_MAX];
  struct stat st;
  jw_t w;

  plat_path("/data/neohen/update_check_off", local, sizeof(local));
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

/* POST /api/launcher/remove
 * and it stays away: NeoWEB does not put it back when it starts */
void
api_launcher_remove(http_req_t *req) {
  char local[PATH_MAX];

  http_drain_body(req);
  (void)plat_app_uninstall(LAUNCHER_ID);
  plat_path("/user/app/" LAUNCHER_ID, local, sizeof(local));
  fs_remove_tree(local, NULL);
  remove_old_system_ex();
  plat_path(LAUNCHER_OFF, local, sizeof(local));
  if(fs_write_file(local, "", 0)) {
    LOG_WARN("launcher: %s could not be written", LAUNCHER_OFF);
  }
  LOG_INFO("launcher: %s removed", LAUNCHER_ID);
  http_send_ok(req);
}
