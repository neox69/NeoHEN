/* NeoWEB - payload manager: the ELF/BIN files in NeoHEN's payloads folder.
 *
 * List them, upload one, start it through the console's ELF loader, stop it,
 * delete it and toggle its auto-start marker. Every route works on a bare
 * file name inside NEOWEB_PAYLOAD_DIR and nowhere else: there is no path in
 * any request.
 *
 * The folder and its side files are NeoHEN's own (see
 * source/bootstrapper/source/bundled_payloads.c and payload_autostart.c):
 *   name.elf             the payload
 *   name.elf.auto_start  start it when NeoHEN boots
 *   name.elf.json        {"version":1,"priority":N,"delay_seconds":N}
 *   name.elf.bundled     NeoHEN installed it and may update it
 *   name.elf.stopped     stopped by hand: NeoHEN's watchdogs leave it be
 *                        (removed when it is started again)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "api.h"
#include "fsutil.h"
#include "log.h"
#include "plat.h"
#include "sha256.h"

#define NEOWEB_PAYLOAD_DIR  "/data/neohen/payloads"
/* An empty file here turns the routes that change anything into 403. */
#define NEOWEB_READONLY     PS5AIO_DATA_DIR "/readonly"
#define PAYLOAD_NAME_MAX    64
#define PAYLOAD_SIZE_MAX    (64LL * 1024 * 1024)
#define UPLOAD_CHUNK        (256 * 1024)
#define MAX_PIDS            16
#define MAX_TRACKED         32

/* Payloads started here whose process does not carry the file's name. */
typedef struct tracked {
  char  name[PAYLOAD_NAME_MAX + 1];
  pid_t pid;
} tracked_t;

static tracked_t       g_tracked[MAX_TRACKED];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------ names, paths */

static int
has_suffix(const char *s, const char *suffix) {
  size_t n = strlen(s), m = strlen(suffix);
  return n > m && !strcmp(s + n - m, suffix);
}

/* A bare file name: letters, digits and . _ + - only, ending in .elf/.bin. */
static int
valid_name(const char *name) {
  size_t n = name ? strlen(name) : 0;
  if(n < 5 || n > PAYLOAD_NAME_MAX || name[0] == '.') {
    return 0;
  }
  for(size_t i = 0; i < n; i++) {
    char c = name[i];
    if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' || c == '-')) {
      return 0;
    }
  }
  return has_suffix(name, ".elf") || has_suffix(name, ".bin");
}

static void
payload_path(const char *name, const char *suffix, char *local, size_t size) {
  char console[PATH_MAX];
  snprintf(console, sizeof(console), NEOWEB_PAYLOAD_DIR "/%s%s", name, suffix);
  plat_path(console, local, size);
}

static int
exists(const char *local) {
  struct stat st;
  return !stat(local, &st);
}

/* replies 403 and returns non-zero when the service is read-only */
static int
read_only(http_req_t *req) {
  char local[PATH_MAX];
  plat_path(NEOWEB_READONLY, local, sizeof(local));
  if(!exists(local)) {
    return 0;
  }
  http_send_error(req, 403, "NeoWEB is read-only (%s exists)", NEOWEB_READONLY);
  return 1;
}

/* name from the JSON body; replies 400 and returns NULL when it is not valid */
static jval_t *
body_name(http_req_t *req, char *name, size_t size) {
  jval_t *b = api_body(req);
  if(!b) {
    return NULL;
  }
  snprintf(name, size, "%s", json_get_str(b, "name", ""));
  if(!valid_name(name)) {
    json_free(b);
    http_send_error(req, 400, "invalid payload name");
    return NULL;
  }
  return b;
}

/* ------------------------------------------------------------ processes */

typedef struct pid_set {
  const char *name; /* NULL: collect every pid */
  pid_t       pids[512];
  int         n;
} pid_set_t;

/* The loader names the process after the file, but the kernel keeps only the
 * first 15 (Linux) or 19 (PS5) characters. */
static int
comm_matches(const char *comm, const char *name) {
  size_t n = strlen(comm);
  if(!strcmp(comm, name)) {
    return 1;
  }
  return n >= 15 && n < strlen(name) && !strncmp(comm, name, n);
}

static int
collect_cb(void *ctx, pid_t pid, const char *comm) {
  pid_set_t *s = ctx;
  if(s->n < (int)(sizeof(s->pids) / sizeof(s->pids[0])) &&
     (!s->name || comm_matches(comm, s->name))) {
    s->pids[s->n++] = pid;
  }
  return 0;
}

static int
in_set(const pid_set_t *s, pid_t pid) {
  for(int i = 0; i < s->n; i++) {
    if(s->pids[i] == pid) {
      return 1;
    }
  }
  return 0;
}

/* pids running this payload: by process name, plus the ones started here */
static int
payload_pids(const char *name, pid_t *out, int max) {
  pid_set_t *byname = calloc(1, sizeof(*byname));
  pid_set_t *all = calloc(1, sizeof(*all));
  int n = 0;

  if(!byname || !all) {
    free(byname);
    free(all);
    return 0;
  }
  byname->name = name;
  plat_proc_each(collect_cb, byname);
  plat_proc_each(collect_cb, all);
  for(int i = 0; i < byname->n && n < max; i++) {
    out[n++] = byname->pids[i];
  }
  pthread_mutex_lock(&g_lock);
  for(int i = 0; i < MAX_TRACKED; i++) {
    tracked_t *t = &g_tracked[i];
    if(!t->pid || strcmp(t->name, name)) {
      continue;
    }
    if(!in_set(all, t->pid)) {
      t->pid = 0; /* it exited */
      continue;
    }
    int dup = 0;
    for(int k = 0; k < n; k++) {
      dup |= out[k] == t->pid;
    }
    if(!dup && n < max) {
      out[n++] = t->pid;
    }
  }
  pthread_mutex_unlock(&g_lock);
  free(byname);
  free(all);
  return n;
}

static void
track(const char *name, pid_t pid) {
  pthread_mutex_lock(&g_lock);
  for(int i = 0; i < MAX_TRACKED; i++) {
    if(!g_tracked[i].pid) {
      snprintf(g_tracked[i].name, sizeof(g_tracked[i].name), "%s", name);
      g_tracked[i].pid = pid;
      break;
    }
  }
  pthread_mutex_unlock(&g_lock);
}

static void
untrack(const char *name) {
  pthread_mutex_lock(&g_lock);
  for(int i = 0; i < MAX_TRACKED; i++) {
    if(!strcmp(g_tracked[i].name, name)) {
      g_tracked[i].pid = 0;
    }
  }
  pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------ resident payloads
 * Some payloads move into a system process and leave none of their own:
 * ps5debug and ps5debug-NG run inside SceShellCore, and only their command
 * port shows they are there. A second copy must not be started over them
 * (ps5debug-NG refuses by itself; Ctn's ps5debug does not). */
static const struct {
  const char *prefix; /* of the file name, any case */
  int         port;
} k_resident[] = {
  {"ps5debug", 744},
};

static int
resident_port(const char *name) {
  for(size_t i = 0; i < sizeof(k_resident) / sizeof(k_resident[0]); i++) {
    if(!strncasecmp(name, k_resident[i].prefix, strlen(k_resident[i].prefix))) {
      return k_resident[i].port;
    }
  }
  return 0;
}

/* something on this console takes connections on 127.0.0.1:port */
static int
port_answers(int port) {
  struct sockaddr_in a;
  struct timeval tv = {1, 0};
  int fd = socket(AF_INET, SOCK_STREAM, 0), ok;

  if(fd < 0) {
    return 0;
  }
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ok = !connect(fd, (struct sockaddr *)&a, sizeof(a));
  close(fd);
  return ok;
}

/* ------------------------------------------------------------ list */

static int
name_cmp(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

static void
write_payload(jw_t *w, const char *name) {
  char local[PATH_MAX];
  struct stat st;
  pid_t pids[MAX_PIDS];
  int n;

  payload_path(name, "", local, sizeof(local));
  if(stat(local, &st) || !S_ISREG(st.st_mode)) {
    return;
  }
  jw_obj(w);
  jw_kstr(w, "name", name);
  jw_kuint(w, "size", (uint64_t)st.st_size);
  jw_kint(w, "modified", (int64_t)st.st_mtime);

  payload_path(name, ".auto_start", local, sizeof(local));
  jw_kbool(w, "autoStart", exists(local));
  payload_path(name, ".bundled", local, sizeof(local));
  jw_kbool(w, "bundled", exists(local));

  {
    char *data = NULL;
    size_t len = 0;
    int priority = 0;
    payload_path(name, ".json", local, sizeof(local));
    if(!fs_read_file(local, &data, &len, 4096)) {
      jval_t *j = json_parse(data, len);
      if(j) {
        priority = (int)json_get_int(j, "priority", 0);
        json_free(j);
      }
      free(data);
    }
    jw_kint(w, "priority", priority);
  }

  n = payload_pids(name, pids, MAX_PIDS);
  jw_kbool(w, "running", n > 0);
  if(resident_port(name)) {
    /* at work inside a system process: "running" is about its own one */
    jw_kbool(w, "serving", port_answers(resident_port(name)));
    jw_kint(w, "port", resident_port(name));
  }
  if(n > 0) {
    int self = 0;
    for(int i = 0; i < n; i++) {
      self |= pids[i] == getpid();
    }
    jw_kint(w, "pid", self ? getpid() : pids[0]);
    /* the service cannot stop itself and survive to answer (any of the
     * processes with this name may be this one: stop refuses them all) */
    jw_kbool(w, "self", self);
  }
  jw_obj_end(w);
}

void
api_payloads_list(http_req_t *req) {
  char local[PATH_MAX];
  char (*names)[PAYLOAD_NAME_MAX + 1] = NULL;
  size_t n = 0, cap = 0;
  struct dirent *e;
  DIR *d;
  jw_t w;

  plat_path(NEOWEB_PAYLOAD_DIR, local, sizeof(local));
  if((d = opendir(local))) {
    while((e = readdir(d))) {
      if(!valid_name(e->d_name)) {
        continue;
      }
      if(n == cap) {
        size_t ncap = cap ? cap * 2 : 32;
        void *p = realloc(names, ncap * sizeof(*names));
        if(!p) {
          break;
        }
        names = p;
        cap = ncap;
      }
      snprintf(names[n++], sizeof(*names), "%s", e->d_name);
    }
    closedir(d);
  }
  if(n) {
    qsort(names, n, sizeof(*names), name_cmp);
  }

  api_begin(&w);
  jw_kstr(&w, "dir", NEOWEB_PAYLOAD_DIR);
  plat_path(NEOWEB_READONLY, local, sizeof(local));
  jw_kbool(&w, "readOnly", exists(local));
  jw_key(&w, "payloads");
  jw_arr(&w);
  for(size_t i = 0; i < n; i++) {
    write_payload(&w, names[i]);
  }
  jw_arr_end(&w);
  free(names);
  api_finish(req, &w);
}

/* ------------------------------------------------------------ upload */

void
api_payloads_upload(http_req_t *req) {
  char name[PAYLOAD_NAME_MAX + 8] = "";
  char dir[PATH_MAX], local[PATH_MAX], tmp[PATH_MAX], stamp[PATH_MAX];
  unsigned char magic[4] = {0};
  uint64_t written = 0;
  int failed = 0;
  char *buf;
  int fd;
  jw_t w;

  if(read_only(req)) {
    return;
  }
  http_query(req, "name", name, sizeof(name));
  if(!valid_name(name)) {
    http_send_error(req, 400, "invalid payload name (letters, digits, . _ + -, "
                              "ending in .elf or .bin)");
    return;
  }
  if(req->content_length <= 0) {
    http_send_error(req, 411, "Content-Length required");
    return;
  }
  if(req->content_length > PAYLOAD_SIZE_MAX) {
    http_send_error(req, 413, "payload larger than %lld MB",
                    PAYLOAD_SIZE_MAX / (1024 * 1024));
    return;
  }

  plat_path(NEOWEB_PAYLOAD_DIR, dir, sizeof(dir));
  fs_mkdirs(dir, 0777);
  payload_path(name, "", local, sizeof(local));
  payload_path(name, ".upload", tmp, sizeof(tmp));

  if((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0777)) < 0) {
    http_send_error(req, 500, "%s: %s", name, strerror(errno));
    return;
  }
  if(!(buf = malloc(UPLOAD_CHUNK))) {
    close(fd);
    unlink(tmp);
    http_send_error(req, 500, "out of memory");
    return;
  }
  while(req->body_left > 0 && !failed) {
    ssize_t got = http_read_body(req, buf, UPLOAD_CHUNK);
    if(got <= 0) {
      failed = 1;
      break;
    }
    if(written < sizeof(magic)) {
      size_t take = sizeof(magic) - (size_t)written;
      memcpy(magic + written, buf, (size_t)got < take ? (size_t)got : take);
    }
    for(char *p = buf; got > 0;) {
      ssize_t put = write(fd, p, (size_t)got);
      if(put <= 0) {
        if(put < 0 && errno == EINTR) {
          continue;
        }
        failed = 2;
        break;
      }
      p += put;
      got -= put;
      written += (uint64_t)put;
    }
  }
  free(buf);
  if(close(fd) && !failed) {
    failed = 2;
  }
  if(failed) {
    unlink(tmp);
    req->keep_alive = 0;
    http_send_error(req, failed == 1 ? 400 : 500, failed == 1
                    ? "upload interrupted" : "could not write the payload");
    return;
  }
  if(has_suffix(name, ".elf") && memcmp(magic, "\x7f" "ELF", 4)) {
    unlink(tmp);
    http_send_error(req, 400, "%s is not an ELF file", name);
    return;
  }
  if(rename(tmp, local)) {
    unlink(tmp);
    http_send_error(req, 500, "%s: %s", name, strerror(errno));
    return;
  }
  /* It is the user's file now: NeoHEN must not replace it with its own. */
  payload_path(name, ".bundled", stamp, sizeof(stamp));
  unlink(stamp);
  LOG_INFO("payload uploaded: %s (%llu bytes) from %s", name,
           (unsigned long long)written, req->client_ip);

  api_begin(&w);
  jw_kstr(&w, "name", name);
  jw_kuint(&w, "size", written);
  api_finish(req, &w);
}

/* ------------------------------------------------------------ start / stop */

void
api_payloads_start(http_req_t *req) {
  char name[PAYLOAD_NAME_MAX + 8], local[PATH_MAX];
  pid_set_t *before, *after;
  pid_t started = 0;
  jval_t *b;
  jw_t w;

  if(read_only(req) || !(b = body_name(req, name, sizeof(name)))) {
    return;
  }
  json_free(b);
  payload_path(name, "", local, sizeof(local));
  if(!exists(local)) {
    http_send_error(req, 404, "%s is not in %s", name, NEOWEB_PAYLOAD_DIR);
    return;
  }
  if(resident_port(name)) {
    pid_t pids[MAX_PIDS];
    if(payload_pids(name, pids, MAX_PIDS) > 0 || port_answers(resident_port(name))) {
      http_send_error(req, 409, "%s is already running (port %d answers): it is not started twice",
                      name, resident_port(name));
      return;
    }
  }
  if(!(before = calloc(1, sizeof(*before))) || !(after = calloc(1, sizeof(*after)))) {
    free(before);
    http_send_error(req, 500, "out of memory");
    return;
  }
  plat_proc_each(collect_cb, before);

  if(plat_payload_launch(local)) {
    free(before);
    free(after);
    http_send_error(req, 502, "%s", plat_error());
    return;
  }
  LOG_INFO("payload started: %s from %s", name, req->client_ip);
  payload_path(name, ".stopped", local, sizeof(local));
  unlink(local);

  /* Find its process: the one named after the file, or failing that the
   * newest process that was not there before. */
  for(int tries = 0; tries < 20 && !started; tries++) {
    pid_t byname[MAX_PIDS];
    int nbyname;
    usleep(150 * 1000);
    /* only the first nbyname entries are filled in */
    if((nbyname = payload_pids(name, byname, MAX_PIDS)) > 0) {
      for(int i = 0; i < nbyname && !started; i++) {
        if(byname[i] && !in_set(before, byname[i])) {
          started = byname[i];
        }
      }
      if(started) {
        break;
      }
    }
    if(tries < 8) {
      continue; /* give it time to rename itself first */
    }
    memset(after, 0, sizeof(*after));
    plat_proc_each(collect_cb, after);
    for(int i = 0; i < after->n; i++) {
      if(!in_set(before, after->pids[i]) && after->pids[i] != getpid() &&
         after->pids[i] > started) {
        started = after->pids[i];
      }
    }
    if(started) {
      track(name, started);
    }
  }
  free(before);
  free(after);

  api_begin(&w);
  jw_kstr(&w, "name", name);
  if(started) {
    jw_kint(&w, "pid", started);
  }
  /* A payload that does its job and exits leaves no process: not an error. */
  jw_kbool(&w, "running", started > 0);
  api_finish(req, &w);
}

void
api_payloads_stop(http_req_t *req) {
  char name[PAYLOAD_NAME_MAX + 8];
  pid_t pids[MAX_PIDS];
  int n, killed = 0;
  jval_t *b;
  jw_t w;

  if(read_only(req) || !(b = body_name(req, name, sizeof(name)))) {
    return;
  }
  json_free(b);
  /* Killing kstuff panics the kernel. It does not live in this folder, but
   * a copy dropped here must not become a way to do it. */
  if(!strncmp(name, "kstuff", 6)) {
    http_send_error(req, 400, "kstuff cannot be stopped: the console would crash");
    return;
  }
  n = payload_pids(name, pids, MAX_PIDS);
  if(!n) {
    http_send_error(req, 409, "%s is not running", name);
    return;
  }
  for(int i = 0; i < n; i++) {
    if(pids[i] == getpid()) {
      http_send_error(req, 400, "NeoWEB cannot stop itself");
      return;
    }
  }
  for(int i = 0; i < n; i++) {
    if(pids[i] > 1 && !kill(pids[i], SIGKILL)) {
      killed++;
    }
  }
  untrack(name);
  if(!killed) {
    http_send_error(req, 500, "could not stop %s", name);
    return;
  }
  LOG_INFO("payload stopped: %s (%d process%s) from %s", name, killed,
           killed == 1 ? "" : "es", req->client_ip);
  {
    /* stopped on purpose: a watchdog must not start it again */
    char marker[PATH_MAX];
    payload_path(name, ".stopped", marker, sizeof(marker));
    fs_write_file(marker, "", 0);
  }
  api_begin(&w);
  jw_kstr(&w, "name", name);
  jw_kint(&w, "stopped", killed);
  api_finish(req, &w);
}

/* ------------------------------------------------------------ delete, auto-start */

void
api_payloads_delete(http_req_t *req) {
  char name[PAYLOAD_NAME_MAX + 8], local[PATH_MAX];
  pid_t pids[MAX_PIDS];
  jval_t *b;

  if(read_only(req) || !(b = body_name(req, name, sizeof(name)))) {
    return;
  }
  json_free(b);
  payload_path(name, "", local, sizeof(local));
  if(!exists(local)) {
    http_send_error(req, 404, "%s is not in %s", name, NEOWEB_PAYLOAD_DIR);
    return;
  }
  if(payload_pids(name, pids, MAX_PIDS) > 0) {
    http_send_error(req, 409, "%s is running: stop it first", name);
    return;
  }
  if(unlink(local)) {
    http_send_error(req, 500, "%s: %s", name, strerror(errno));
    return;
  }
  payload_path(name, ".auto_start", local, sizeof(local));
  unlink(local);
  payload_path(name, ".json", local, sizeof(local));
  unlink(local);
  payload_path(name, ".stopped", local, sizeof(local));
  unlink(local);
  /* The ".bundled" stamp stays on purpose: it is what tells NeoHEN that this
   * payload was removed by the user and must not be installed again. */
  LOG_INFO("payload deleted: %s from %s", name, req->client_ip);
  http_send_ok(req);
}

void
api_payloads_autostart(http_req_t *req) {
  char name[PAYLOAD_NAME_MAX + 8], local[PATH_MAX], marker[PATH_MAX];
  jval_t *b;
  int on;
  jw_t w;

  if(read_only(req) || !(b = body_name(req, name, sizeof(name)))) {
    return;
  }
  on = json_get_bool(b, "enabled", 1);
  json_free(b);
  payload_path(name, "", local, sizeof(local));
  if(!exists(local)) {
    http_send_error(req, 404, "%s is not in %s", name, NEOWEB_PAYLOAD_DIR);
    return;
  }
  payload_path(name, ".auto_start", marker, sizeof(marker));
  if(on) {
    if(fs_write_file(marker, "", 0)) {
      http_send_error(req, 500, "%s: %s", name, strerror(errno));
      return;
    }
  } else if(unlink(marker) && errno != ENOENT) {
    http_send_error(req, 500, "%s: %s", name, strerror(errno));
    return;
  }
  api_begin(&w);
  jw_kstr(&w, "name", name);
  jw_kbool(&w, "autoStart", on);
  api_finish(req, &w);
}

/* ------------------------------------------------------------ health
 * Two things the dashboard warns about (see docs/robustness.md):
 *   - safe mode: NeoHEN failed to reach "boot OK" 3 starts in a row, so this
 *     start skipped auto-start payloads and plugins. /data/neohen/safe_mode
 *     says so, /data/neohen/boot_attempts counts the failed starts. Clearing
 *     it here removes both (the next start is a normal one).
 *   - conflicts: another homebrew stack running beside NeoHEN. Only a
 *     warning: nothing is stopped. */
#define NEOHEN_SAFE_MODE     "/data/neohen/safe_mode"
#define NEOHEN_BOOT_ATTEMPTS "/data/neohen/boot_attempts"

typedef struct conflict_scan {
  int kstuff;
  unsigned found; /* bit per k_conflicts entry */
} conflict_scan_t;

/* lowercase piece of the process name, and what to call it */
static const char *const k_conflicts[][2] = {
  {"backpork", "BackPork"},
  {"etahen", "etaHEN"},
  {"cheatrunner", "CheatRunner"},
};

static int
conflict_cb(void *ctx, pid_t pid, const char *name) {
  conflict_scan_t *c = ctx;
  char low[64];
  size_t i;

  for(i = 0; name[i] && i < sizeof(low) - 1; i++) {
    low[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
  }
  low[i] = 0;
  if(pid == getpid()) {
    return 0;
  }
  if(strstr(low, "kstuff")) {
    c->kstuff++;
  }
  for(i = 0; i < sizeof(k_conflicts) / sizeof(k_conflicts[0]); i++) {
    if(strstr(low, k_conflicts[i][0])) {
      c->found |= 1u << i;
    }
  }
  return 0;
}

/* writes the names of what clashes with NeoHEN, as a JSON array */
void
api_health_conflicts(jw_t *w) {
  conflict_scan_t c = {0};

  plat_proc_each(conflict_cb, &c);
  jw_arr(w);
  for(size_t i = 0; i < sizeof(k_conflicts) / sizeof(k_conflicts[0]); i++) {
    if(c.found & (1u << i)) {
      jw_str(w, k_conflicts[i][1]);
    }
  }
  /* NeoHEN loads one kstuff; a second one (a payload manager's) clashes */
  if(c.kstuff > 1) {
    jw_str(w, "kstuff x2");
  }
  jw_arr_end(w);
}

/* {"active": bool, "failedBoots": n}, as members of the open object */
void
api_health_safe_mode(jw_t *w) {
  char local[PATH_MAX];
  char *text = NULL;
  size_t len = 0;
  int64_t n = 0;

  plat_path(NEOHEN_BOOT_ATTEMPTS, local, sizeof(local));
  if(!fs_read_file(local, &text, &len, 64)) {
    n = atoll(text);
    free(text);
  }
  plat_path(NEOHEN_SAFE_MODE, local, sizeof(local));
  jw_kbool(w, "active", exists(local));
  jw_kint(w, "failedBoots", n > 0 ? n : 0);
}

/* GET /api/health   {"safeMode": {"active", "failedBoots"}, "conflicts": [...]} */
void
api_health(http_req_t *req) {
  jw_t w;

  api_begin(&w);
  jw_key(&w, "safeMode");
  jw_obj(&w);
  api_health_safe_mode(&w);
  jw_obj_end(&w);
  jw_key(&w, "conflicts");
  api_health_conflicts(&w);
  api_finish(req, &w);
}

/* POST /api/health/safemode {}   the next NeoHEN start is a normal one */
void
api_health_safe_mode_clear(http_req_t *req) {
  char local[PATH_MAX];

  http_drain_body(req);
  if(read_only(req)) {
    return;
  }
  plat_path(NEOHEN_SAFE_MODE, local, sizeof(local));
  if(unlink(local) && errno != ENOENT) {
    http_send_error(req, 500, "%s: %s", NEOHEN_SAFE_MODE, strerror(errno));
    return;
  }
  plat_path(NEOHEN_BOOT_ATTEMPTS, local, sizeof(local));
  unlink(local);
  LOG_INFO("safe mode cleared from %s", req->client_ip);
  http_send_ok(req);
}

/* For the diagnostic report: every payload file with its size, SHA-256,
 * auto-start, the release the store put (store.json) or the version NeoHEN
 * bundled (.bundled stamp). A JSON array. */
void
api_payloads_manifest(jw_t *w) {
  char dir[PATH_MAX], local[PATH_MAX], stamp[PATH_MAX];
  char *text = NULL;
  size_t len = 0;
  jval_t *store = NULL;
  const jval_t *installed = NULL;
  struct dirent *e;
  DIR *d;

  plat_path(PS5AIO_DATA_DIR "/store.json", local, sizeof(local));
  if(!fs_read_file(local, &text, &len, 1 << 20)) {
    store = json_parse(text, len);
    free(text);
    installed = store ? json_get(store, "installed") : NULL;
  }
  jw_arr(w);
  plat_path(NEOWEB_PAYLOAD_DIR, dir, sizeof(dir));
  if((d = opendir(dir))) {
    while((e = readdir(d))) {
      struct stat st;
      if(!valid_name(e->d_name)) {
        continue;
      }
      payload_path(e->d_name, "", local, sizeof(local));
      if(stat(local, &st) || !S_ISREG(st.st_mode)) {
        continue;
      }
      jw_obj(w);
      jw_kstr(w, "name", e->d_name);
      jw_kint(w, "size", (int64_t)st.st_size);
      jw_kint(w, "mtime", (int64_t)st.st_mtime);
      {
        sha256_t s;
        char hex[65] = "", buf[64 * 1024];
        ssize_t n;
        int fd = open(local, O_RDONLY);
        if(fd >= 0) {
          sha256_init(&s);
          while((n = read(fd, buf, sizeof(buf))) > 0) {
            sha256_update(&s, buf, (size_t)n);
          }
          close(fd);
          if(!n) {
            sha256_final(&s, hex);
          }
        }
        jw_kstr(w, "sha256", hex);
      }
      payload_path(e->d_name, ".auto_start", stamp, sizeof(stamp));
      jw_kbool(w, "autoStart", exists(stamp));
      payload_path(e->d_name, ".bundled", stamp, sizeof(stamp));
      if(!fs_read_file(stamp, &text, &len, 256)) {
        while(len && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
          text[--len] = 0;
        }
        jw_kstr(w, "bundled", text);
        free(text);
      }
      if(installed && json_get(installed, e->d_name)) {
        jw_kstr(w, "store", json_get_str(installed, e->d_name, ""));
      }
      jw_obj_end(w);
    }
    closedir(d);
  }
  jw_arr_end(w);
  json_free(store);
}
