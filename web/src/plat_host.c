/* PS5AIO - host (Linux) emulation backend.
 *
 * Lets the whole server run on a PC for development and automated tests:
 * console paths are mapped below $PS5AIO_ROOT (default ./fakeps5), process
 * memory goes through /proc/<pid>/mem and app management is simulated.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "log.h"
#include "plat.h"

static char            g_root[PATH_MAX] = "./fakeps5";
static char            g_running[16];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void
mkdirs(const char *path) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for(char *p = tmp + 1; *p; p++) {
    if(*p == '/') {
      *p = 0;
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
  mkdir(tmp, 0755);
}

int
plat_init(void) {
  /* same process name as on the console, where the payload names itself */
  prctl(PR_SET_NAME, "neoweb.elf");
  static const char *dirs[] = {
    "/user/app", "/user/appmeta", "/user/home", "/data", "/mnt/usb0",
    "/system_data/priv/mms", "/mnt/sandbox/pfsmnt",
  };
  const char *env = getenv("PS5AIO_ROOT");
  if(env && *env) {
    snprintf(g_root, sizeof(g_root), "%s", env);
  }
  {
    char abs[PATH_MAX];
    mkdirs(g_root);
    if(realpath(g_root, abs)) {
      snprintf(g_root, sizeof(g_root), "%s", abs);
    }
  }
  for(size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s%s", g_root, dirs[i]);
    mkdirs(p);
  }
  LOG_INFO("host emulation, console root: %s", g_root);
  return 0;
}

const char *
plat_name(void) {
  return "host";
}

void
plat_path(const char *in, char *out, size_t size) {
  if(!in || !*in) {
    in = "/";
  }
  snprintf(out, size, "%s%s%s", g_root, in[0] == '/' ? "" : "/", in);
  /* strip trailing slash (except root) */
  size_t n = strlen(out);
  while(n > 1 && out[n - 1] == '/') {
    out[--n] = 0;
  }
}

void
plat_notify(const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  LOG_INFO("[notification] %s", msg);
}

void
plat_sensors(jw_t *w) {
  FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
  int t = 0;
  if(f) {
    if(fscanf(f, "%d", &t) == 1) {
      jw_kint(w, "cpuTemp", t / 1000);
      jw_kint(w, "socTemp", t / 1000);
    }
    fclose(f);
  }
  jw_kint(w, "cpuFreqMhz", 3500);
  jw_kint(w, "fanDuty", 37);
}

void
plat_sysinfo(jw_t *w) {
  struct utsname un;
  struct sysinfo si;

  jw_kstr(w, "platform", "host");
  jw_kstr(w, "firmware", "0.00");
  jw_khex(w, "firmwareRaw", 0);
  if(!uname(&un)) {
    char model[512];
    snprintf(model, sizeof(model), "Emulator (%s %s)", un.sysname, un.machine);
    jw_kstr(w, "model", model);
  }
  if(!sysinfo(&si)) {
    jw_kuint(w, "memTotal", (uint64_t)si.totalram * si.mem_unit);
    jw_kint(w, "uptime", si.uptime);
  }
  plat_sensors(w);
  jw_key(w, "ips");
  jw_arr(w);
  jw_obj(w);
  jw_kstr(w, "iface", "lo");
  jw_kstr(w, "ip", "127.0.0.1");
  jw_obj_end(w);
  jw_arr_end(w);
  jw_kstr(w, "root", g_root);
}

void
plat_mounts(jw_t *w) {
  static const char *mps[] = {"/", "/user", "/data", "/mnt/usb0"};
  jw_arr(w);
  for(size_t i = 0; i < sizeof(mps) / sizeof(mps[0]); i++) {
    char p[PATH_MAX];
    struct statvfs sv;
    plat_path(mps[i], p, sizeof(p));
    if(statvfs(p, &sv)) {
      continue;
    }
    jw_obj(w);
    jw_kstr(w, "path", mps[i]);
    jw_kstr(w, "device", "emulated");
    jw_kstr(w, "fs", "hostfs");
    jw_kuint(w, "total", (uint64_t)sv.f_blocks * sv.f_frsize);
    jw_kuint(w, "free", (uint64_t)sv.f_bavail * sv.f_frsize);
    jw_kbool(w, "readonly", 0);
    jw_obj_end(w);
  }
  jw_arr_end(w);
}

void
plat_procs(jw_t *w) {
  DIR *d = opendir("/proc");
  struct dirent *e;

  jw_arr(w);
  if(!d) {
    jw_arr_end(w);
    return;
  }
  while((e = readdir(d))) {
    char path[300], comm[64] = "", state = '?';
    long ppid = 0, rss = 0;
    unsigned long vsize = 0;
    FILE *f;
    pid_t pid = (pid_t)atoi(e->d_name);

    if(pid <= 0) {
      continue;
    }
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if(!(f = fopen(path, "r"))) {
      continue;
    }
    if(fscanf(f, "%*d (%63[^)]) %c %ld %*d %*d %*d %*d %*u %*u %*u %*u %*u "
                 "%*u %*u %*d %*d %*d %*d %*d %*d %*u %lu %ld",
              comm, &state, &ppid, &vsize, &rss) < 3) {
      fclose(f);
      continue;
    }
    fclose(f);

    jw_obj(w);
    jw_kint(w, "pid", pid);
    jw_kint(w, "ppid", ppid);
    jw_kint(w, "uid", 0);
    jw_kstr(w, "name", comm);
    jw_kstr(w, "state", state == 'R' ? "run" : state == 'S' ? "sleep" : "wait");
    jw_kuint(w, "rss", (uint64_t)rss * 4096);
    jw_kuint(w, "vsize", vsize);
    jw_kint(w, "appId", 0);
    jw_obj_end(w);
  }
  closedir(d);
  jw_arr_end(w);
}

void
plat_proc_each(plat_proc_cb cb, void *ctx) {
  DIR *d = opendir("/proc");
  struct dirent *e;

  if(!d) {
    return;
  }
  while((e = readdir(d))) {
    char path[300], comm[64] = "", state = '?';
    pid_t pid = (pid_t)atoi(e->d_name);
    FILE *f;
    int got;

    if(pid <= 0) {
      continue;
    }
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if(!(f = fopen(path, "r"))) {
      continue;
    }
    got = fscanf(f, "%*d (%63[^)]) %c", comm, &state);
    fclose(f);
    /* a killed child stays as a zombie until it is reaped: it is not running */
    if(got < 2 || state == 'Z') {
      continue;
    }
    if(cb(ctx, pid, comm)) {
      break;
    }
  }
  closedir(d);
}

/* Emulated loader: a child process named after the file that just waits, so
 * the payload manager has a real process to find and to kill. */
int
plat_payload_launch(const char *local_path) {
  const char *name = strrchr(local_path, '/');
  struct stat st;
  pid_t pid;

  if(stat(local_path, &st)) {
    plat_set_error("%s: %s", local_path, strerror(errno));
    return -1;
  }
  name = name ? name + 1 : local_path;
  signal(SIGCHLD, SIG_IGN); /* no zombies */
  if((pid = fork()) < 0) {
    plat_set_error("fork: %s", strerror(errno));
    return -1;
  }
  if(!pid) {
    /* PR_SET_PDEATHSIG would fire when the request thread that forked us
     * ends, so watch the server process instead. */
    pid_t server = getppid();
    prctl(PR_SET_NAME, name);
    while(getppid() == server) {
      sleep(1);
    }
    _exit(0);
  }
  LOG_INFO("[emu] payload %s started as pid %d", name, pid);
  return 0;
}

int
plat_proc_title(pid_t pid, char *title_id, size_t size) {
  (void)pid;
  (void)title_id;
  (void)size;
  return -1;
}

int
plat_proc_read(pid_t pid, uint64_t addr, void *buf, size_t len) {
  char path[64];
  int fd;
  snprintf(path, sizeof(path), "/proc/%d/mem", pid);
  if((fd = open(path, O_RDONLY)) < 0) {
    plat_set_error("%s: %s", path, strerror(errno));
    return -1;
  }
  ssize_t n = pread(fd, buf, len, (off_t)addr);
  close(fd);
  if(n != (ssize_t)len) {
    plat_set_error("read @0x%lx failed", (unsigned long)addr);
    return -1;
  }
  return 0;
}

int
plat_proc_write(pid_t pid, uint64_t addr, const void *buf, size_t len) {
  char path[64];
  int fd;
  snprintf(path, sizeof(path), "/proc/%d/mem", pid);
  if((fd = open(path, O_RDWR)) < 0) {
    plat_set_error("%s: %s", path, strerror(errno));
    return -1;
  }
  ssize_t n = pwrite(fd, buf, len, (off_t)addr);
  close(fd);
  if(n != (ssize_t)len) {
    plat_set_error("write @0x%lx failed", (unsigned long)addr);
    return -1;
  }
  return 0;
}

int
plat_proc_maps(pid_t pid, plat_map_cb cb, void *ctx) {
  char path[64], line[1024];
  FILE *f;
  snprintf(path, sizeof(path), "/proc/%d/maps", pid);
  if(!(f = fopen(path, "r"))) {
    plat_set_error("%s: %s", path, strerror(errno));
    return -1;
  }
  while(fgets(line, sizeof(line), f)) {
    unsigned long start, end;
    char perms[8] = "", name[512] = "";
    int prot = 0;
    if(sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &start, &end, perms,
              name) < 3) {
      continue;
    }
    if(perms[0] == 'r') prot |= PLAT_PROT_READ;
    if(perms[1] == 'w') prot |= PLAT_PROT_WRITE;
    if(perms[2] == 'x') prot |= PLAT_PROT_EXEC;
    char *n = name;
    while(*n == ' ') {
      n++;
    }
    if(cb(ctx, start, end, prot, n)) {
      break;
    }
  }
  fclose(f);
  return 0;
}

uint64_t
plat_proc_generation(pid_t pid) {
  return pid > 0 ? 1 : 0;
}

int
plat_app_running(char *title_id, size_t size, int *app_id, pid_t *pid) {
  pthread_mutex_lock(&g_lock);
  int running = g_running[0] != 0;
  if(running && title_id) {
    snprintf(title_id, size, "%s", g_running);
  }
  pthread_mutex_unlock(&g_lock);
  if(!running) {
    plat_set_error("no application running");
    return -1;
  }
  if(app_id) {
    *app_id = 0x60000001;
  }
  if(pid) {
    *pid = getpid();
  }
  return 0;
}

int
plat_app_kill_running(void) {
  pthread_mutex_lock(&g_lock);
  int running = g_running[0] != 0;
  g_running[0] = 0;
  pthread_mutex_unlock(&g_lock);
  if(!running) {
    plat_set_error("no application running");
    return -1;
  }
  return 0;
}

int
plat_app_launch(const char *title_id, const char *args) {
  char p[PATH_MAX], lp[64];
  struct stat st;
  snprintf(lp, sizeof(lp), "/user/app/%s", title_id);
  plat_path(lp, p, sizeof(p));
  if(stat(p, &st)) {
    plat_set_error("title %s is not installed", title_id);
    return -1;
  }
  pthread_mutex_lock(&g_lock);
  snprintf(g_running, sizeof(g_running), "%s", title_id);
  pthread_mutex_unlock(&g_lock);
  LOG_INFO("[emu] launched %s %s", title_id, args ? args : "");
  return 0;
}

static int
copy_file(const char *src, const char *dst) {
  char buf[65536];
  int in = open(src, O_RDONLY), out;
  ssize_t n;
  if(in < 0) {
    return -1;
  }
  if((out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
    close(in);
    return -1;
  }
  while((n = read(in, buf, sizeof(buf))) > 0) {
    if(write(out, buf, (size_t)n) != n) {
      break;
    }
  }
  close(in);
  close(out);
  return 0;
}

int
plat_app_install_dir(const char *title_id, const char *dir) {
  static const char *files[] = {"param.json", "icon0.png", "pic0.png", "pic1.png", "param.sfo"};
  char src[PATH_MAX], dst[PATH_MAX], lp[PATH_MAX];
  struct stat st;

  snprintf(lp, sizeof(lp), "%s/%s", dir, title_id);
  plat_path(lp, src, sizeof(src));
  if(stat(src, &st) || !S_ISDIR(st.st_mode)) {
    plat_set_error("%s/%s does not exist", dir, title_id);
    return -1;
  }
  snprintf(lp, sizeof(lp), "/user/appmeta/%s", title_id);
  plat_path(lp, dst, sizeof(dst));
  mkdirs(dst);
  for(size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    char s[PATH_MAX + 64], d[PATH_MAX + 64];
    snprintf(lp, sizeof(lp), "%s/%s/sce_sys/%s", dir, title_id, files[i]);
    plat_path(lp, s, sizeof(s));
    snprintf(d, sizeof(d), "%s/%s", dst, files[i]);
    copy_file(s, d);
  }
  LOG_INFO("[emu] installed %s from %s", title_id, dir);
  return 0;
}

int
plat_app_install_pkg(const char *url, const char *name, const char *icon_url,
                     char *content_id, size_t size) {
  (void)icon_url;
  LOG_INFO("[emu] install pkg %s (%s)", url, name ? name : "");
  if(content_id) {
    snprintf(content_id, size, "EMU000-EMUL00000_00-0000000000000000");
  }
  return 0;
}

int
plat_unmount(const char *path) {
  /* the emulator's drives are folders: a "busy" file inside one says no */
  char p[PATH_MAX], busy[PATH_MAX];
  struct stat st;
  plat_path(path, p, sizeof(p));
  snprintf(busy, sizeof(busy), "%s/.busy", p);
  if(!stat(busy, &st)) {
    plat_set_error("Device busy");
    return -1;
  }
  LOG_INFO("[emu] unmount %s (simulated)", path);
  return 0;
}

int
plat_first_ip(char *out, size_t size) {
  snprintf(out, size, "127.0.0.1");
  return 0;
}

int
plat_remount_system_ex(void) {
  return 0; /* the emulator's /system_ex is an ordinary folder */
}

int
plat_app_uninstall(const char *title_id) {
  char p[PATH_MAX], lp[64];
  struct stat st;
  snprintf(lp, sizeof(lp), "/user/appmeta/%s", title_id);
  plat_path(lp, p, sizeof(p));
  if(stat(p, &st)) {
    plat_set_error("title %s is not installed", title_id);
    return -1;
  }
  LOG_INFO("[emu] uninstall %s (simulated, files kept)", title_id);
  return 0;
}

int
plat_eject_disc(void) {
  LOG_INFO("[emu] eject disc");
  return 0;
}

int
plat_power(const char *action) {
  LOG_INFO("[emu] power action: %s", action);
  if(strcmp(action, "rest") && strcmp(action, "reboot") && strcmp(action, "poweroff")) {
    plat_set_error("unknown power action '%s'", action);
    return -1;
  }
  return 0;
}

int
plat_open_browser(const char *url) {
  LOG_INFO("[emu] open browser: %s", url);
  return 0;
}
