/* NeoHEN themes service.
 *
 * Keeps the theme named in /data/neohen/themes/active.txt shown on top of
 * the menu's view of /system_ex/vsh_asset (home music, backgrounds), with a
 * READ-ONLY unionfs mount, the mechanism ShadowMount+ uses for its backport
 * overlays. An empty or missing active.txt means the stock look.
 *
 * SceShellUI (NPXS40087) reads its assets through its own sandbox, a nullfs
 * view of /system_ex made when it starts, so the theme goes on that path and
 * is put again whenever the menu restarts (Rest Mode, a NeoHEN reload).
 *
 * The one rule: nothing here writes below /system or /system_ex. A mount
 * lives in the kernel's mount table, so a reboot always brings the stock
 * files back, untouched.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>

#define THEMES   "/data/neohen/themes"
#define ACTIVE   THEMES "/active.txt"
#define STATE    THEMES "/state.txt"
#define SANDBOX  "/mnt/sandbox"
#define MENU     "NPXS40087_"
#define ASSETS   "/system_ex/vsh_asset"
#define SELF     "neotheme.elf"
#define POLL_SECONDS 3

typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void
say(const char *fmt, ...) {
  notify_request_t req;
  va_list ap;

  memset(&req, 0, sizeof(req));
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof(req.message), fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* What the web page and the Toolbox read: the theme that is really on. */
static void
write_state(const char *theme, const char *detail) {
  FILE *f = fopen(STATE, "w");
  if(f) {
    fprintf(f, "%s\n%s\n", theme, detail);
    fclose(f);
  }
}

static void
kill_previous(void) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  pid_t self = getpid();
  char *buf;

  if(sysctl(mib, 4, NULL, &size, NULL, 0)) {
    return;
  }
  size += 16 * sizeof(struct kinfo_proc);
  if(!(buf = malloc(size)) || sysctl(mib, 4, buf, &size, NULL, 0)) {
    free(buf);
    return;
  }
  for(char *ptr = buf; ptr < buf + size;) {
    struct kinfo_proc *ki = (struct kinfo_proc *)ptr;
    if(ki->ki_structsize <= 0) {
      break;
    }
    ptr += ki->ki_structsize;
    if(ki->ki_pid != self && (!strcmp(ki->ki_tdname, SELF) || !strcmp(ki->ki_comm, SELF))) {
      kill(ki->ki_pid, SIGKILL);
      usleep(300 * 1000);
    }
  }
  free(buf);
}

/* The folder of the theme to show, or "" for the stock look. Only a plain
 * folder name inside THEMES is accepted. */
static void
read_active(char *path, size_t size, char *name, size_t name_size) {
  struct stat st;
  FILE *f;

  path[0] = name[0] = 0;
  if(!(f = fopen(ACTIVE, "r"))) {
    return;
  }
  if(!fgets(name, (int)name_size, f)) {
    name[0] = 0;
  }
  fclose(f);
  name[strcspn(name, "\r\n")] = 0;
  if(!name[0] || name[0] == '.' || strstr(name, "..")) {
    name[0] = 0;
    return;
  }
  for(const char *p = name; *p; p++) {
    if(!isalnum((unsigned char)*p) && !strchr(" ._-()", *p)) {
      name[0] = 0;
      return;
    }
  }
  snprintf(path, size, THEMES "/%s", name);
  if(stat(path, &st) || !S_ISDIR(st.st_mode)) {
    path[0] = name[0] = 0;
  }
}

/* Is a theme of ours on this folder? If so, which one (its source path). */
static int
mounted(const char *target, char *source, size_t size) {
  struct statfs sfs;

  if(statfs(target, &sfs) || strcmp(sfs.f_fstypename, "unionfs") ||
     strcmp(sfs.f_mntonname, target)) {
    return 0;
  }
  const char *from = sfs.f_mntfromname;
  if(!strncmp(from, "<above>:", 8)) {
    from += 8;
  }
  snprintf(source, size, "%s", from);
  return 1;
}

#define IOV(s) {(void *)(s), (s) ? strlen(s) + 1 : 0}

static int
put(const char *theme, const char *target) {
  struct iovec iov[] = {
      IOV("fstype"),   IOV("unionfs"),
      IOV("from"),     IOV(theme),
      IOV("fspath"),   IOV(target),
      IOV("copymode"), IOV("transparent"),
      IOV("noatime"),  IOV(NULL),
      IOV("fnodup"),   IOV(NULL),
  };
  /* MNT_RDONLY: nothing can be written through the mount, in either layer */
  return nmount(iov, sizeof(iov) / sizeof(iov[0]), MNT_RDONLY);
}

/* Brings every live menu sandbox to the wanted theme ("" = none).
 * Returns the number of sandboxes that are as wanted, -1 if one is not yet. */
static int
reconcile(const char *theme) {
  struct dirent *e;
  int ok = 0, pending = 0;
  DIR *dir = opendir(SANDBOX);

  if(!dir) {
    return -1;
  }
  while((e = readdir(dir))) {
    char target[512], current[512];
    struct stat st;

    if(strncmp(e->d_name, MENU, strlen(MENU))) {
      continue;
    }
    snprintf(target, sizeof(target), SANDBOX "/%s" ASSETS, e->d_name);
    if(stat(target, &st)) {
      continue; /* the sandbox of a menu that is gone */
    }
    int on = mounted(target, current, sizeof(current));
    if(on && !strcmp(current, theme)) {
      ok++;
      continue;
    }
    /* Never forced: if the menu has a file of the old theme open (the music
     * that is playing), this is tried again on the next round. */
    if(on && unmount(target, 0)) {
      pending++;
      continue;
    }
    if(!theme[0] || !put(theme, target)) {
      ok++;
    } else {
      pending++;
    }
  }
  closedir(dir);
  return pending ? -1 : ok;
}

int
main(void) {
  char shown[512] = "\x01"; /* nothing reported yet */

  kill_previous();
  syscall(SYS_thr_set_name, -1, SELF);
  mkdir(THEMES, 0777);

  for(;;) {
    char theme[512], name[128];

    read_active(theme, sizeof(theme), name, sizeof(name));
    int r = reconcile(theme);
    if(r >= 0 && strcmp(shown, theme)) {
      /* said once per change, and only when it is really in place */
      if(theme[0]) {
        say("NeoHEN: tema \"%s\" aplicado", name);
      } else if(shown[0] != 1) {
        say("NeoHEN: aspecto de fabrica");
      }
      snprintf(shown, sizeof(shown), "%s", theme);
      write_state(name, "ok");
    } else if(r < 0 && strcmp(shown, theme)) {
      write_state(name, "pending");
    }
    sleep(POLL_SECONDS);
  }
  return 0;
}
