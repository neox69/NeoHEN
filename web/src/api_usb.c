/* NeoWEB - taking a USB drive away safely.
 *
 * Pulling a drive out from under the console can hang it: ShadowMount+
 * mounts the drive's images and also shows the drive inside the sandbox of
 * the app that is open. So, in this order: the app that is open is closed
 * (only when the caller says so), ShadowMount+ lets go of every image it
 * has mounted from that drive, and then the drive itself is unmounted. If
 * something still holds it the unmount fails and nothing is forced.
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

/* The drive the console calls /mnt/usbN, if that is what name is. */
static int
usb_drive(const char *name, char *mnt, size_t size) {
  if(strncmp(name, "usb", 3) || name[3] < '0' || name[3] > '7' || name[4]) {
    return -1;
  }
  snprintf(mnt, size, "/mnt/%s", name);
  return 0;
}

/* A drive is there when its folder is a mount point (the emulator has no
 * mounts: there the folder being there is enough). */
static int
is_mount_point(const char *mnt) {
  char local[PATH_MAX], up[PATH_MAX];
  struct stat a, b;

  plat_path(mnt, local, sizeof(local));
  if(stat(local, &a) || !S_ISDIR(a.st_mode)) {
    return 0;
  }
  if(strcmp(plat_name(), "ps5")) {
    return 1;
  }
  plat_path("/mnt", up, sizeof(up));
  return !stat(up, &b) && a.st_dev != b.st_dev;
}

/* Asks ShadowMount+ to unmount the images it has from that drive; how many. */
static int
smp_release(const char *mnt, char *err, size_t esize) {
  char body[64];
  jval_t *list, *games;
  size_t n = strlen(mnt);
  int count = 0;

  if(smp_call("/api/v1/games", "{\"include_size\":false}", &list, err, esize)) {
    return -1; /* not running: it holds nothing */
  }
  games = json_get(list, "games");
  for(const jval_t *g = games && games->type == J_ARR ? games->child : NULL; g; g = g->next) {
    const char *path = json_get_str(g, "path", "");
    const char *tid = json_get_str(g, "title_id", "");

    if(!json_get_bool(g, "mounted", 0) || strncmp(path, mnt, n) || path[n] != '/') {
      continue;
    }
    snprintf(body, sizeof(body), "{\"title_id\":\"%s\"}", tid);
    if(smp_call("/api/v1/games/unmount", body, NULL, err, esize)) {
      LOG_WARN("usb: ShadowMount+ kept %s mounted: %s", tid, err);
      json_free(list);
      return -2;
    }
    count++;
  }
  json_free(list);
  return count;
}

/* POST /api/usb/eject {"drive": "usb0", "closeApp": false} */
void
api_usb_eject(http_req_t *req) {
  char mnt[16], tid[16] = "", err[160];
  jval_t *b = api_body(req);
  int close_app, released;
  jw_t w;

  if(!b) {
    return;
  }
  if(usb_drive(json_get_str(b, "drive", ""), mnt, sizeof(mnt))) {
    json_free(b);
    http_send_error(req, 400, "drive must be usb0 .. usb7");
    return;
  }
  close_app = json_get_bool(b, "closeApp", 0);
  json_free(b);
  if(!is_mount_point(mnt)) {
    http_send_error(req, 404, "there is no drive in %s", mnt);
    return;
  }
  /* an open app may see the drive inside its sandbox */
  if(!plat_app_running(tid, sizeof(tid), NULL, NULL) && tid[0]) {
    if(!close_app) {
      http_send_error(req, 409, "%s is open and may be using the drive: close it first", tid);
      return;
    }
    if(plat_app_kill_running()) {
      http_send_error(req, 500, "%s could not be closed: %s", tid, plat_error());
      return;
    }
    for(int i = 0; i < 50 && !plat_app_running(tid, sizeof(tid), NULL, NULL) && tid[0]; i++) {
      usleep(100 * 1000);
    }
    sleep(1); /* ShadowMount+ takes its sandbox mounts away as the app goes */
  }
  released = smp_release(mnt, err, sizeof(err));
  if(released == -2) {
    http_send_error(req, 409, "ShadowMount+ did not let go of a game on %s: %s", mnt, err);
    return;
  }
  sync();
  if(plat_unmount(mnt)) {
    http_send_error(req, 409, "%s is still in use: %s", mnt, plat_error());
    return;
  }
  LOG_INFO("usb: %s unmounted (%d image%s let go)", mnt, released > 0 ? released : 0,
           released == 1 ? "" : "s");
  api_begin(&w);
  jw_kstr(&w, "drive", mnt);
  jw_kint(&w, "imagesReleased", released > 0 ? released : 0);
  api_finish(req, &w);
}
