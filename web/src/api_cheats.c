/* NeoWEB - cheats of the game that is open, switched from the page.
 *
 * NeoWEB applies nothing by itself: it asks NeoHEN's own cheat service, the
 * one the Toolbox uses, over the same local socket and with the same two
 * requests. A cheat turned on here shows as on in the Toolbox and the other
 * way round, because there is one session per running game and both ask
 * for it.
 *
 *   list    {"tid", "mode": "runtime", "pid", "appid", "generation"}
 *           answers with the path of a file holding the list, which the
 *           one who asked reads and removes
 *   toggle  {"session_id", "cheat_key", "enabled"}
 *
 * The service puts a notice on the screen when a game has no cheat file, so
 * it is only asked when there is one for the game.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "fsutil.h"
#include "log.h"
#include "plat.h"

#define UTIL_SOCKET "/system_tmp/neohen/ipc/util_service"
#define CHEATS_DIR  "/data/neohen/cheats"
#define IPC_MAGIC   0xDEADBABEu
#define CMD_LIST    0x800000Au /* BREW_UTIL_GET_GAME_CHEAT */
#define CMD_TOGGLE  0x800000Bu /* BREW_UTIL_TOGGLE_CHEAT */
#define TIMEOUT_S   15

/* the frame both ways: struct IPCMessage of source/include/msg.hpp */
typedef struct ipc_msg {
  uint32_t magic;
  uint32_t cmd;
  int32_t  error;
  char     msg[0x1000];
} ipc_msg_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int
io_all(int fd, void *buf, size_t len, int sending) {
  char *p = buf;

  while(len) {
    ssize_t n = sending ? send(fd, p, len, 0) : recv(fd, p, len, 0);
    if(n < 0 && errno == EINTR) {
      continue;
    }
    if(n <= 0) {
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

/* Ask the service. 0: it said yes, 1: it said no, -1: it did not answer.
 * `var` gets what came back with the answer, if anything. */
static int
ipc_call(uint32_t cmd, const jw_t *request, char *var, size_t size) {
  struct timeval tv = {TIMEOUT_S, 0};
  struct sockaddr_un addr;
  char local[PATH_MAX];
  ipc_msg_t *m;
  jval_t *reply;
  int fd, rc = -1;

  var[0] = 0;
  plat_path(UTIL_SOCKET, local, sizeof(local));
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if(request->oom || request->len >= sizeof(m->msg) || strlen(local) >= sizeof(addr.sun_path) ||
     !(m = calloc(1, sizeof(*m)))) {
    return -1;
  }
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", local);
  m->magic = IPC_MAGIC;
  m->cmd = cmd;
  memcpy(m->msg, request->data, request->len);

  pthread_mutex_lock(&g_lock);
  if((fd = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0) {
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if(!connect(fd, (struct sockaddr *)&addr,
                (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(addr.sun_path) + 1)) && !io_all(fd, m, sizeof(*m), 1) &&
       !io_all(fd, m, sizeof(*m), 0) && m->magic == IPC_MAGIC) {
      m->msg[sizeof(m->msg) - 1] = 0;
      rc = m->error ? 1 : 0;
      if((reply = json_parse(m->msg, strlen(m->msg)))) {
        snprintf(var, size, "%s", json_get_str(reply, "var", ""));
        json_free(reply);
      }
    }
    close(fd);
  }
  pthread_mutex_unlock(&g_lock);
  free(m);
  return rc;
}

/* how many cheat files there are for a game */
static int
cheat_files(const char *tid) {
  char local[PATH_MAX];
  struct dirent *e;
  int n = 0;
  DIR *dir;

  plat_path(CHEATS_DIR, local, sizeof(local));
  if((dir = opendir(local))) {
    while((e = readdir(dir))) {
      n += !strncmp(e->d_name, tid, 9) && e->d_name[9] == '_';
    }
    closedir(dir);
  }
  return n;
}

/* GET /api/cheats/live
 * the cheats of the game that is open, with which ones are on */
void
api_cheats_live(http_req_t *req) {
  char tid[16] = "", var[PATH_MAX], local[PATH_MAX], gen[32];
  int app_id = 0, files, rc;
  pid_t pid = -1;
  jw_t w, q;

  api_begin(&w);
  if(plat_app_running(tid, sizeof(tid), &app_id, &pid) || !is_title_id(tid) ||
     !strncmp(tid, "NPXS", 4)) {
    jw_kbool(&w, "running", 0);
    api_finish(req, &w);
    return;
  }
  files = cheat_files(tid);
  jw_kbool(&w, "running", 1);
  jw_kstr(&w, "titleId", tid);
  jw_kint(&w, "files", files);
  if(files) {
    snprintf(gen, sizeof(gen), "%llu", (unsigned long long)plat_proc_generation(pid));
    jw_init(&q);
    jw_obj(&q);
    jw_kstr(&q, "tid", tid);
    jw_kstr(&q, "mode", "runtime");
    jw_kint(&q, "pid", pid);
    jw_kint(&q, "appid", app_id);
    jw_kstr(&q, "generation", gen);
    jw_obj_end(&q);
    rc = ipc_call(CMD_LIST, &q, var, sizeof(var));
    jw_free(&q);
    if(rc < 0) {
      jw_kstr(&w, "error", "NeoHEN's cheat service is not answering");
    } else if(rc || var[0] != '/') {
      jw_kstr(&w, "error", "the cheat service has no cheats it can apply to this game");
    } else {
      char *text = NULL;
      size_t len = 0;
      jval_t *list;

      plat_path(var, local, sizeof(local));
      if(!fs_read_file(local, &text, &len, 4 << 20) && (list = json_parse(text, len))) {
        jw_key(&w, "list");
        json_write(&w, list);
        json_free(list);
      } else {
        jw_kstr(&w, "error", "the list the cheat service wrote could not be read");
      }
      free(text);
      unlink(local);
    }
  }
  api_finish(req, &w);
}

/* POST /api/cheats/toggle {"session": "...", "key": "...", "enabled": true} */
void
api_cheats_toggle(http_req_t *req) {
  const char *session, *key;
  char var[1024];
  jval_t *b;
  jw_t w, q;
  int rc;

  if(!(b = api_body(req))) {
    return;
  }
  session = json_get_str(b, "session", "");
  key = json_get_str(b, "key", "");
  if(!session[0] || !key[0] || !json_get(b, "enabled")) {
    json_free(b);
    http_send_error(req, 400, "session, key and enabled are needed");
    return;
  }
  jw_init(&q);
  jw_obj(&q);
  jw_kstr(&q, "session_id", session);
  jw_kstr(&q, "cheat_key", key);
  jw_kbool(&q, "enabled", json_get_bool(b, "enabled", 0));
  jw_obj_end(&q);
  rc = ipc_call(CMD_TOGGLE, &q, var, sizeof(var));
  jw_free(&q);
  LOG_INFO("cheats: %s %s -> %s", json_get_bool(b, "enabled", 0) ? "on" : "off", key,
           rc < 0 ? "no answer" : var);
  json_free(b);
  if(rc < 0) {
    http_send_error(req, 502, "NeoHEN's cheat service is not answering");
  } else if(rc) {
    http_send_error(req, 409, "%s", var[0] && strcmp(var, "Nothing") ? var : "the cheat could not be changed");
  } else {
    api_begin(&w);
    jw_kstr(&w, "status", var);
    api_finish(req, &w);
  }
}
