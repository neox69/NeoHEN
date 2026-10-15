/* PS5AIO - PS5 platform backend (ps5-payload-sdk).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/reboot.h>
#include <sys/sysctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/mdbg.h>

#include "log.h"
#include "plat.h"

/* ------------------------------------------------------------ SCE prototypes */

typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

typedef struct app_info {
  uint32_t app_id;
  uint64_t unknown1;
  uint32_t app_type;
  char     title_id[10];
  char     unknown2[0x3c];
} app_info_t;

typedef struct app_launch_ctx {
  uint32_t structsize;
  uint32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
} app_launch_ctx_t;

typedef struct pkg_metadata {
  const char *uri;
  const char *ex_uri;
  const char *playgo_scenario_id;
  const char *content_id;
  const char *content_name;
  const char *icon_url;
} pkg_metadata_t;

typedef struct pkg_info {
  char content_id[48];
  int  type;
  int  platform;
} pkg_info_t;

typedef struct playgo_info {
  char lang[8][30];
  char scenario_ids[3][64];
  char content_ids[64];
  long unknown[810];
} playgo_info_t;

int  sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
int  sceKernelGetAppInfo(pid_t pid, app_info_t *info);
int  sceKernelGetHwModelName(char *);
int  sceKernelGetHwSerialNumber(char *);
long sceKernelGetCpuFrequency(void);
int  sceKernelGetCpuTemperature(int *);
int  sceKernelGetSocSensorTemperature(int, int *);
int  sceKernelGetCurrentFanDuty(uint16_t *, uint64_t *);
/* the console's own way of going to Rest Mode, restarting and turning off:
 * it closes what is open and flushes the disks first */
int  sceShellCoreUtilRequestEjectDevice(const char *);
int  sceSystemStateMgrEnterStandby(int);
int  sceSystemStateMgrReboot(void);
int  sceSystemStateMgrTurnOff(void);

int  sceUserServiceInitialize(void *);
int  sceUserServiceGetForegroundUser(uint32_t *user_id);
int  sceUserServiceGetUserName(uint32_t user_id, char *name, size_t size);

int  sceSystemServiceGetAppIdOfRunningBigApp(void);
int  sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
int  sceSystemServiceLaunchApp(const char *title_id, char **argv, app_launch_ctx_t *ctx);
int  sceSystemServiceLaunchWebBrowser(const char *uri, void *);

int  sceAppInstUtilInitialize(void);
int  sceAppInstUtilAppInstallAll(void *);
int  sceAppInstUtilInstallByPackage(const pkg_metadata_t *, pkg_info_t *, playgo_info_t *);
int  sceAppInstUtilAppUnInstall(const char *title_id);

static int g_appinst_ready;

/* Title id of an app_info (4 upper-case letters and 5 digits).
 *
 * The field moved between firmwares: the SDK's struct has it at offset 20,
 * but on 13.42 it starts at 16 and reading the struct member returns the
 * id without its first four letters ("40087" for NPXS40087). Look for the
 * pattern at both offsets instead of trusting one layout. */
static int
app_title(const app_info_t *ai, char *out, size_t size) {
  static const size_t offsets[] = {16, 20};
  const char *raw = (const char *)ai;

  for(size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
    const char *t = raw + offsets[i];
    int ok = 1;
    for(int c = 0; c < 9 && ok; c++) {
      ok = c < 4 ? (t[c] >= 'A' && t[c] <= 'Z') : (t[c] >= '0' && t[c] <= '9');
    }
    if(ok) {
      snprintf(out, size, "%.9s", t);
      return 0;
    }
  }
  if(size) {
    out[0] = 0;
  }
  return -1;
}

static void *proc_list(size_t *size);

/* ------------------------------------------------------------ basics */

/* kill instances of the payload that were loaded before us */
static void
kill_previous(void) {
  size_t size = 0;
  void *buf = proc_list(&size);
  pid_t self = getpid();

  if(!buf) {
    return;
  }
  for(char *ptr = buf; ptr < (char *)buf + size;) {
    struct kinfo_proc *ki = (struct kinfo_proc *)ptr;
    if(ki->ki_structsize <= 0) {
      break;
    }
    ptr += ki->ki_structsize;
    if(ki->ki_pid != self && (!strcmp(ki->ki_tdname, "neoweb.elf") ||
                              !strcmp(ki->ki_comm, "neoweb.elf"))) {
      LOG_INFO("stopping previous instance (pid %d)", ki->ki_pid);
      kill(ki->ki_pid, SIGKILL);
      usleep(500 * 1000);
    }
  }
  free(buf);
}

int
plat_init(void) {
  int err;

  kill_previous();
  syscall(SYS_thr_set_name, -1, "neoweb.elf");

  if((err = sceUserServiceInitialize(0))) {
    LOG_WARN("sceUserServiceInitialize: 0x%x", err);
  }

  /* grant ourselves a system-level auth id (same as websrv/ftpsrv) so that
   * SystemService/AppInstUtil calls are accepted */
  kernel_set_ucred_authid(getpid(), 0x4801000000000013L);

#ifdef NEOWEB_FULL
  /* only the installer routes need it */
  if((err = sceAppInstUtilInitialize())) {
    LOG_WARN("sceAppInstUtilInitialize: 0x%x", err);
  } else {
    g_appinst_ready = 1;
  }
#endif
  return 0;
}

const char *
plat_name(void) {
  return "ps5";
}

void
plat_path(const char *in, char *out, size_t size) {
  snprintf(out, size, "%s", in && *in ? in : "/");
}

void
plat_notify(const char *fmt, ...) {
  notify_request_t req;
  va_list ap;

  memset(&req, 0, sizeof(req));
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof(req.message), fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* ------------------------------------------------------------ info */

static void
write_ips(jw_t *w) {
  struct ifaddrs *ifaddr;

  jw_key(w, "ips");
  jw_arr(w);
  if(getifaddrs(&ifaddr) == 0) {
    for(struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
      char ip[INET_ADDRSTRLEN];
      if(!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
        continue;
      }
      if(!strncmp(ifa->ifa_name, "lo", 2)) {
        continue;
      }
      inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, ip,
                sizeof(ip));
      if(!strncmp(ip, "0.", 2)) {
        continue;
      }
      jw_obj(w);
      jw_kstr(w, "iface", ifa->ifa_name);
      jw_kstr(w, "ip", ip);
      jw_obj_end(w);
    }
    freeifaddrs(ifaddr);
  }
  jw_arr_end(w);
}

void
plat_sensors(jw_t *w) {
  int temp = 0;

  if(!sceKernelGetCpuTemperature(&temp)) {
    jw_kint(w, "cpuTemp", temp);
  }
  if(!sceKernelGetSocSensorTemperature(0, &temp)) {
    jw_kint(w, "socTemp", temp);
  }
  jw_kint(w, "cpuFreqMhz", sceKernelGetCpuFrequency() / (1000 * 1000));
  {
    uint16_t duty = 0;
    uint64_t chassis = 0;
    if(!sceKernelGetCurrentFanDuty(&duty, &chassis)) {
      jw_kint(w, "fanDuty", (duty * 100 + 512) / 1024); /* percent */
    }
  }
  /* every sensor of the SoC; the first one is socTemp */
  jw_key(w, "socSensors");
  jw_arr(w);
  for(int i = 0; i < 12; i++) {
    if(sceKernelGetSocSensorTemperature(i, &temp)) {
      break;
    }
    jw_int(w, temp);
  }
  jw_arr_end(w);
}

void
plat_sysinfo(jw_t *w) {
  char buf[256];
  uint32_t fw = kernel_get_fw_version();

  jw_kstr(w, "platform", "ps5");

  snprintf(buf, sizeof(buf), "%x.%02x", (fw >> 24) & 0xff, (fw >> 16) & 0xff);
  jw_kstr(w, "firmware", buf);
  jw_khex(w, "firmwareRaw", fw);

  memset(buf, 0, sizeof(buf));
  if(!sceKernelGetHwModelName(buf)) {
    jw_kstr(w, "model", buf);
  }
  /* No serial number, user id or account name: this is served to the LAN. */

  {
    uint64_t physmem = 0;
    size_t len = sizeof(physmem);
    if(!sysctlbyname("hw.physmem", &physmem, &len, NULL, 0)) {
      jw_kuint(w, "memTotal", physmem);
    }
  }
  {
    struct timeval bt;
    size_t len = sizeof(bt);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if(!sysctl(mib, 2, &bt, &len, NULL, 0)) {
      jw_kint(w, "uptime", (int64_t)(time(NULL) - bt.tv_sec));
    }
  }

  plat_sensors(w);
  write_ips(w);
}

void
plat_mounts(jw_t *w) {
  struct statfs *mnts = NULL;
  int n = getmntinfo(&mnts, MNT_NOWAIT);

  jw_arr(w);
  for(int i = 0; i < n; i++) {
    jw_obj(w);
    jw_kstr(w, "path", mnts[i].f_mntonname);
    jw_kstr(w, "device", mnts[i].f_mntfromname);
    jw_kstr(w, "fs", mnts[i].f_fstypename);
    jw_kuint(w, "total", (uint64_t)mnts[i].f_blocks * mnts[i].f_bsize);
    jw_kuint(w, "free", (uint64_t)mnts[i].f_bavail * mnts[i].f_bsize);
    jw_kbool(w, "readonly", (mnts[i].f_flags & MNT_RDONLY) != 0);
    jw_obj_end(w);
  }
  jw_arr_end(w);
}

/* ------------------------------------------------------------ processes */

static void *
proc_list(size_t *size) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  void *buf;

  if(sysctl(mib, 4, NULL, size, NULL, 0)) {
    return NULL;
  }
  *size += 16 * sizeof(struct kinfo_proc);
  if(!(buf = malloc(*size))) {
    return NULL;
  }
  if(sysctl(mib, 4, buf, size, NULL, 0)) {
    free(buf);
    return NULL;
  }
  return buf;
}

void
plat_procs(jw_t *w) {
  static const char *states[] = {"", "start", "run", "sleep", "stop", "zombie", "wait", "lock"};
  size_t size = 0;
  void *buf = proc_list(&size);

  jw_arr(w);
  if(buf) {
    for(char *ptr = buf; ptr < (char *)buf + size;) {
      struct kinfo_proc *ki = (struct kinfo_proc *)ptr;
      app_info_t ai;

      if(ki->ki_structsize <= 0) {
        break;
      }
      ptr += ki->ki_structsize;

      memset(&ai, 0, sizeof(ai));
      sceKernelGetAppInfo(ki->ki_pid, &ai);

      jw_obj(w);
      jw_kint(w, "pid", ki->ki_pid);
      jw_kint(w, "ppid", ki->ki_ppid);
      jw_kint(w, "uid", ki->ki_uid);
      jw_kstr(w, "name", ki->ki_comm);
      jw_kstr(w, "state", states[(unsigned)ki->ki_stat & 7]);
      jw_kuint(w, "rss", (uint64_t)ki->ki_rssize * PAGE_SIZE);
      jw_kuint(w, "vsize", (uint64_t)ki->ki_size);
      jw_kint(w, "appId", ai.app_id);
      {
        char tid[16];
        if(!app_title(&ai, tid, sizeof(tid))) {
          jw_kstr(w, "titleId", tid);
        }
      }
      jw_obj_end(w);
    }
    free(buf);
  }
  jw_arr_end(w);
}

void
plat_proc_each(plat_proc_cb cb, void *ctx) {
  size_t size = 0;
  void *buf = proc_list(&size);

  if(!buf) {
    return;
  }
  for(char *ptr = buf; ptr < (char *)buf + size;) {
    struct kinfo_proc *ki = (struct kinfo_proc *)ptr;
    if(ki->ki_structsize <= 0) {
      break;
    }
    ptr += ki->ki_structsize;
    if(cb(ctx, ki->ki_pid, ki->ki_comm)) {
      break;
    }
  }
  free(buf);
}

/* The ELF loader (elfldr) takes a payload as a raw stream on port 9021 and
 * runs it in a new process. From the console itself it is always reachable,
 * including the loaders that only accept local connections. */
int
plat_payload_launch(const char *local_path) {
  struct sockaddr_in addr;
  char buf[65536];
  ssize_t n;
  int file, sock;

  if((file = open(local_path, O_RDONLY)) < 0) {
    plat_set_error("%s: %s", local_path, strerror(errno));
    return -1;
  }
  if((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    plat_set_error("socket: %s", strerror(errno));
    close(file);
    return -1;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9021);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if(connect(sock, (struct sockaddr *)&addr, sizeof(addr))) {
    plat_set_error("the ELF loader is not listening on port 9021 (%s)",
                   strerror(errno));
    close(sock);
    close(file);
    return -1;
  }
  while((n = read(file, buf, sizeof(buf))) > 0) {
    for(char *p = buf; n > 0;) {
      ssize_t sent = send(sock, p, (size_t)n, 0);
      if(sent <= 0) {
        if(sent < 0 && errno == EINTR) {
          continue;
        }
        plat_set_error("sending to the ELF loader: %s", strerror(errno));
        close(sock);
        close(file);
        return -1;
      }
      p += sent;
      n -= sent;
    }
  }
  close(sock);
  close(file);
  return 0;
}

int
plat_proc_title(pid_t pid, char *title_id, size_t size) {
  app_info_t ai;
  memset(&ai, 0, sizeof(ai));
  if(sceKernelGetAppInfo(pid, &ai)) {
    return -1;
  }
  return app_title(&ai, title_id, size);
}

/* ------------------------------------------------------------ memory */

int
plat_proc_read(pid_t pid, uint64_t addr, void *buf, size_t len) {
  if(mdbg_copyout(pid, (intptr_t)addr, buf, len)) {
    plat_set_error("read %zu bytes @0x%lx in pid %d failed", len, (unsigned long)addr, pid);
    return -1;
  }
  return 0;
}

int
plat_proc_write(pid_t pid, uint64_t addr, const void *buf, size_t len) {
  if(!mdbg_copyin(pid, buf, (intptr_t)addr, len)) {
    return 0;
  }
  /* fall back to a kernel-level write, which ignores page protection */
  if(!kernel_proc_copyin(pid, buf, (intptr_t)addr, len)) {
    return 0;
  }
  plat_set_error("write %zu bytes @0x%lx in pid %d failed", len, (unsigned long)addr, pid);
  return -1;
}

int
plat_proc_maps(pid_t pid, plat_map_cb cb, void *ctx) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, pid};
  size_t size = 0;
  char *buf;

  if(sysctl(mib, 4, NULL, &size, NULL, 0)) {
    plat_set_error("sysctl(VMMAP, %d): %s", pid, strerror(errno));
    return -1;
  }
  size = size * 4 / 3 + 4096;
  if(!(buf = malloc(size))) {
    plat_set_error("out of memory");
    return -1;
  }
  if(sysctl(mib, 4, buf, &size, NULL, 0)) {
    plat_set_error("sysctl(VMMAP, %d): %s", pid, strerror(errno));
    free(buf);
    return -1;
  }

  for(char *p = buf; p < buf + size;) {
    struct kinfo_vmentry *e = (struct kinfo_vmentry *)p;
    int prot = 0;

    if(e->kve_structsize <= 0) {
      break;
    }
    p += e->kve_structsize;

    if(e->kve_protection & KVME_PROT_READ)  prot |= PLAT_PROT_READ;
    if(e->kve_protection & KVME_PROT_WRITE) prot |= PLAT_PROT_WRITE;
    if(e->kve_protection & KVME_PROT_EXEC)  prot |= PLAT_PROT_EXEC;

    if(cb(ctx, e->kve_start, e->kve_end, prot, e->kve_path)) {
      break;
    }
  }
  free(buf);
  return 0;
}

/* ------------------------------------------------------------ apps */

static pid_t
find_app_pid(int app_id, char *title_id, size_t size) {
  size_t bsize = 0;
  void *buf = proc_list(&bsize);
  pid_t found = -1;

  if(!buf) {
    return -1;
  }
  for(char *ptr = buf; ptr < (char *)buf + bsize;) {
    struct kinfo_proc *ki = (struct kinfo_proc *)ptr;
    app_info_t ai;
    if(ki->ki_structsize <= 0) {
      break;
    }
    ptr += ki->ki_structsize;
    memset(&ai, 0, sizeof(ai));
    if(sceKernelGetAppInfo(ki->ki_pid, &ai)) {
      continue;
    }
    if((int)ai.app_id == app_id) {
      if(title_id) {
        app_title(&ai, title_id, size);
      }
      /* prefer the eboot process, which is usually the first match */
      found = ki->ki_pid;
      break;
    }
  }
  free(buf);
  return found;
}

/* The running "big app" (game or homebrew).
 *
 * sceSystemServiceGetAppIdOfRunningBigApp() is 0 on some firmwares even with
 * a game open (seen on 13.42), so fall back to the process list: every
 * process that belongs to an app has an app id and a title id, and anything
 * that is not a system title (NPXS...) is the big app. With several
 * candidates the most recently started one wins; its lowest pid is reported,
 * which is the eboot. Returns the app id, or 0 when nothing is running. */
static int
running_big_app(char *title_id, size_t size, pid_t *pid) {
  int id = sceSystemServiceGetAppIdOfRunningBigApp();
  size_t bsize = 0;
  void *buf;
  pid_t newest = -1;
  pid_t first = -1;
  char tid[16] = "";

  if(id > 0) {
    pid_t p = find_app_pid(id, title_id, size);
    if(pid) {
      *pid = p;
    }
    return id;
  }
  id = 0;
  if(!(buf = proc_list(&bsize))) {
    return 0;
  }
  for(int pass = 0; pass < 2; pass++) {
    for(char *ptr = buf; ptr < (char *)buf + bsize;) {
      struct kinfo_proc *ki = (struct kinfo_proc *)ptr;
      app_info_t ai;
      char cur[16];
      if(ki->ki_structsize <= 0) {
        break;
      }
      ptr += ki->ki_structsize;
      memset(&ai, 0, sizeof(ai));
      if(sceKernelGetAppInfo(ki->ki_pid, &ai) || !ai.app_id ||
         app_title(&ai, cur, sizeof(cur)) || !strncmp(cur, "NPXS", 4)) {
        continue;
      }
      if(pass == 0) {
        if(ki->ki_pid > newest) {
          newest = ki->ki_pid;
          id = (int)ai.app_id;
          snprintf(tid, sizeof(tid), "%s", cur);
        }
      } else if((int)ai.app_id == id && (first < 0 || ki->ki_pid < first)) {
        first = ki->ki_pid;
      }
    }
  }
  free(buf);
  if(id > 0) {
    if(title_id) {
      snprintf(title_id, size, "%s", tid);
    }
    if(pid) {
      *pid = first;
    }
  }
  return id;
}

uint64_t
plat_proc_generation(pid_t pid) {
  uint64_t gen = 0;
  size_t size = 0;
  void *buf;

  /* from the whole list, like the Toolbox does: the console's record of a
   * process is not the size the headers say, and each one tells its own */
  if(pid <= 0 || !(buf = proc_list(&size))) {
    return 0;
  }
  for(char *ptr = buf; ptr < (char *)buf + size;) {
    struct kinfo_proc *ki = (struct kinfo_proc *)ptr;
    if(ki->ki_structsize <= 0) {
      break;
    }
    ptr += ki->ki_structsize;
    if(ki->ki_pid == pid) {
      gen = (uint64_t)ki->ki_start.tv_sec << 32 | (uint64_t)ki->ki_start.tv_usec;
      break;
    }
  }
  free(buf);
  return gen;
}

int
plat_app_running(char *title_id, size_t size, int *app_id, pid_t *pid) {
  int id = running_big_app(title_id, size, pid);
  if(id <= 0) {
    plat_set_error("no application running");
    return -1;
  }
  if(app_id) {
    *app_id = id;
  }
  return 0;
}

int
plat_app_kill_running(void) {
  int id = running_big_app(NULL, 0, NULL);
  int err;
  if(id <= 0) {
    plat_set_error("no application running");
    return -1;
  }
  if((err = sceSystemServiceKillApp(id, -1, 0, 0))) {
    plat_set_error("sceSystemServiceKillApp: 0x%x", err);
    return -1;
  }
  return 0;
}

int
plat_app_launch(const char *title_id, const char *args) {
  app_launch_ctx_t ctx;
  char *argv[32];
  char *copy = NULL;
  int argc = 0;
  int err;

  memset(&ctx, 0, sizeof(ctx));
  memset(argv, 0, sizeof(argv));

  if((err = sceUserServiceGetForegroundUser(&ctx.user_id))) {
    plat_set_error("sceUserServiceGetForegroundUser: 0x%x", err);
    return -1;
  }

  /* only one "big app" may run at a time */
  if(running_big_app(NULL, 0, NULL) > 0) {
    plat_app_kill_running();
    usleep(500 * 1000);
  }

  if(args && *args && (copy = strdup(args))) {
    char *save = NULL;
    for(char *tok = strtok_r(copy, " ", &save); tok && argc < 31;
        tok = strtok_r(NULL, " ", &save)) {
      argv[argc++] = tok;
    }
  }

  err = sceSystemServiceLaunchApp(title_id, argv, &ctx);
  free(copy);
  if(err < 0) {
    plat_set_error("sceSystemServiceLaunchApp(%s): 0x%x", title_id, err);
    return -1;
  }
  return 0;
}

int
plat_app_install_dir(const char *title_id, const char *dir) {
  int (*install_title_dir)(const char *, const char *, void *) = 0;
  uint32_t handle;
  int err;

  if(!g_appinst_ready) {
    plat_set_error("AppInstUtil not initialized");
    return -1;
  }
  /* sceAppInstUtilAppInstallTitleDir is resolved by NID because it is not
   * part of the stub library on every SDK version */
  if(!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle)) {
    install_title_dir = (void *)kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
  }
  if(install_title_dir) {
    err = install_title_dir(title_id, dir, 0);
  } else {
    err = sceAppInstUtilAppInstallAll(0);
  }
  if(err) {
    plat_set_error("install %s from %s: 0x%x", title_id, dir, err);
    return -1;
  }
  return 0;
}

int
plat_app_install_pkg(const char *url, const char *name, const char *icon_url,
                     char *content_id, size_t size) {
  playgo_info_t *playgo;
  pkg_info_t pkginfo;
  pkg_metadata_t meta = {
    .uri = url,
    .ex_uri = "",
    .playgo_scenario_id = "",
    .content_id = "",
    .content_name = name ? name : "",
    .icon_url = icon_url ? icon_url : "",
  };
  int err;

  if(!g_appinst_ready) {
    plat_set_error("AppInstUtil not initialized");
    return -1;
  }
  if(!(playgo = calloc(1, sizeof(*playgo)))) {
    plat_set_error("out of memory");
    return -1;
  }
  memset(&pkginfo, 0, sizeof(pkginfo));
  err = sceAppInstUtilInstallByPackage(&meta, &pkginfo, playgo);
  free(playgo);
  if(err) {
    plat_set_error("sceAppInstUtilInstallByPackage: 0x%x", err);
    return -1;
  }
  if(content_id) {
    snprintf(content_id, size, "%.47s", pkginfo.content_id);
  }
  return 0;
}

int
plat_app_uninstall(const char *title_id) {
  int err;
  if(!g_appinst_ready) {
    plat_set_error("AppInstUtil not initialized");
    return -1;
  }
  if((err = sceAppInstUtilAppUnInstall(title_id))) {
    plat_set_error("sceAppInstUtilAppUnInstall(%s): 0x%x", title_id, err);
    return -1;
  }
  return 0;
}

/* ------------------------------------------------------------ power */

/* The disc drive is /dev/cd0; the system is asked to let go of it, as it
 * does when the eject button is pressed. */
int
plat_eject_disc(void) {
  int r = sceShellCoreUtilRequestEjectDevice("/dev/cd0");
  if(r) {
    plat_set_error("the console refused to eject the disc (0x%x)", r);
    return -1;
  }
  return 0;
}

int
plat_power(const char *action) {
  int r;

  sync();
  if(!strcmp(action, "rest")) {
    r = sceSystemStateMgrEnterStandby(0);
  } else if(!strcmp(action, "reboot")) {
    r = sceSystemStateMgrReboot();
  } else if(!strcmp(action, "poweroff")) {
    r = sceSystemStateMgrTurnOff();
  } else {
    plat_set_error("unknown power action '%s'", action);
    return -1;
  }
  if(r) {
    plat_set_error("the console refused to %s (0x%x)", action, r);
    return -1;
  }
  return 0;
}

int
plat_unmount(const char *path) {
  if(unmount(path, 0)) {
    plat_set_error("%s", strerror(errno));
    return -1;
  }
  return 0;
}

int
plat_remount_system_ex(void) {
  struct iovec iov[] = {
#define IOV(x) {(void *)(x), (x) ? strlen(x) + 1 : 0}
    IOV("from"),      IOV("/dev/ssd0.system_ex"),
    IOV("fspath"),    IOV("/system_ex"),
    IOV("fstype"),    IOV("exfatfs"),
    IOV("large"),     IOV("yes"),
    IOV("timezone"),  IOV("static"),
    IOV("async"),     IOV(NULL),
    IOV("ignoreacl"), IOV(NULL),
#undef IOV
  };
  if(nmount(iov, sizeof(iov) / sizeof(iov[0]), MNT_UPDATE)) {
    plat_set_error("remount /system_ex: %s", strerror(errno));
    return -1;
  }
  return 0;
}

int
plat_open_browser(const char *url) {
  int err = sceSystemServiceLaunchWebBrowser(url, 0);
  if(err) {
    plat_set_error("sceSystemServiceLaunchWebBrowser: 0x%x", err);
    return -1;
  }
  return 0;
}
