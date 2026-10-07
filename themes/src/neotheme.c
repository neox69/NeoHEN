/* NeoHEN themes service.
 *
 * Keeps the theme named in /data/neohen/themes/active.txt shown on top of
 * the menu's view of /system_ex/vsh_asset (home music, backgrounds). An
 * empty or missing active.txt means the stock look.
 *
 * How: the folder themes/.current is put over the menu's assets once, with
 * a READ-ONLY unionfs mount (the mechanism ShadowMount+ uses for its
 * backport overlays), and it is never taken off: the menu keeps files of it
 * open, and an unmount fails while it does. Changing theme only changes
 * what is inside .current, which holds hard links to the active theme's
 * files; empty, the menu sees the stock files.
 *
 * SceShellUI (NPXS40087) reads its assets through its own sandbox, a nullfs
 * view of /system_ex made when it starts, so the mount goes on that path and
 * is made again whenever the menu restarts (Rest Mode, a NeoHEN reload).
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
#define CURRENT  THEMES "/.current"
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

/* Is .current already on this folder? */
static int
mounted(const char *target) {
  struct statfs sfs;

  if(statfs(target, &sfs) || strcmp(sfs.f_fstypename, "unionfs") ||
     strcmp(sfs.f_mntonname, target)) {
    return 0;
  }
  const char *from = sfs.f_mntfromname;
  if(!strncmp(from, "<above>:", 8)) {
    from += 8;
  }
  return !strcmp(from, CURRENT);
}

#define IOV(s) {(void *)(s), (s) ? strlen(s) + 1 : 0}

/* Puts .current over the assets of every live menu. Returns how many menus
 * have it, -1 if one could not get it. */
static int
cover_menus(void) {
  struct dirent *e;
  int ok = 0, failed = 0;
  DIR *dir = opendir(SANDBOX);

  if(!dir) {
    return -1;
  }
  while((e = readdir(dir))) {
    char target[512];
    struct stat st;

    if(strncmp(e->d_name, MENU, strlen(MENU))) {
      continue;
    }
    snprintf(target, sizeof(target), SANDBOX "/%s" ASSETS, e->d_name);
    if(stat(target, &st)) {
      continue; /* the sandbox of a menu that is gone */
    }
    if(mounted(target)) {
      ok++;
      continue;
    }
    struct iovec iov[] = {
        IOV("fstype"),   IOV("unionfs"),
        IOV("from"),     IOV(CURRENT),
        IOV("fspath"),   IOV(target),
        IOV("copymode"), IOV("transparent"),
        IOV("noatime"),  IOV(NULL),
        IOV("fnodup"),   IOV(NULL),
    };
    /* MNT_RDONLY: nothing can be written through the mount, in either layer */
    if(nmount(iov, sizeof(iov) / sizeof(iov[0]), MNT_RDONLY)) {
      failed++;
    } else {
      ok++;
    }
  }
  closedir(dir);
  return failed ? -1 : ok;
}

/* A theme's own files that are not for the console. */
static int
skipped(const char *name) {
  return name[0] == '.' || !strcmp(name, "preview.jpg") || !strcmp(name, "theme.json");
}

static int
copy_file(const char *from, const char *to) {
  char buf[65536];
  size_t n;
  int ok = 1;
  FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;

  if(!out) {
    if(in) {
      fclose(in);
    }
    return -1;
  }
  while((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if(fwrite(buf, 1, n, out) != n) {
      ok = 0;
      break;
    }
  }
  fclose(in);
  return fclose(out) || !ok ? -1 : 0;
}

/* Makes .current hold exactly the files of the theme ("" = none). Files are
 * hard links, so nothing is copied and a theme takes no extra space. */
static int
fill_current(const char *theme) {
  char a[768], b[768];
  struct dirent *e;
  int failed = 0;
  DIR *dir;

  mkdir(CURRENT, 0777);
  if((dir = opendir(CURRENT))) {
    while((e = readdir(dir))) {
      if(strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
        snprintf(a, sizeof(a), CURRENT "/%s", e->d_name);
        unlink(a);
      }
    }
    closedir(dir);
  }
  if(!theme[0]) {
    return 0;
  }
  if(!(dir = opendir(theme))) {
    return -1;
  }
  while((e = readdir(dir))) {
    struct stat st;
    if(skipped(e->d_name)) {
      continue;
    }
    snprintf(a, sizeof(a), "%s/%s", theme, e->d_name);
    snprintf(b, sizeof(b), CURRENT "/%s", e->d_name);
    if(stat(a, &st) || !S_ISREG(st.st_mode)) {
      continue;
    }
    if(link(a, b) && copy_file(a, b)) {
      failed++;
    }
  }
  closedir(dir);
  return failed ? -1 : 0;
}

/* Changes when the theme changes, and when a file of it is added, removed
 * or written again (a new music file uploaded over the old one). */
static void
signature(const char *theme, char *out, size_t size) {
  unsigned long long bytes = 0;
  long newest = 0;
  int files = 0;
  struct dirent *e;
  DIR *dir;

  if(!theme[0] || !(dir = opendir(theme))) {
    snprintf(out, size, "-");
    return;
  }
  while((e = readdir(dir))) {
    char path[768];
    struct stat st;
    if(skipped(e->d_name)) {
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", theme, e->d_name);
    if(!stat(path, &st) && S_ISREG(st.st_mode)) {
      files++;
      bytes += (unsigned long long)st.st_size;
      if((long)st.st_mtime > newest) {
        newest = (long)st.st_mtime;
      }
    }
  }
  closedir(dir);
  snprintf(out, size, "%s|%d|%llu|%ld", theme, files, bytes, newest);
}

int
main(void) {
  char shown[640] = ""; /* signature of what .current holds */
  int said = 0;

  kill_previous();
  syscall(SYS_thr_set_name, -1, SELF);
  mkdir(THEMES, 0777);
  mkdir(CURRENT, 0777);

  for(;;) {
    char theme[512], name[128], sig[640];

    read_active(theme, sizeof(theme), name, sizeof(name));
    signature(theme, sig, sizeof(sig));
    if(strcmp(sig, shown)) {
      if(fill_current(theme)) {
        write_state(name, "error");
      } else {
        /* said once per change; nothing is said for the stock look at start */
        if(theme[0]) {
          say("NeoHEN: tema \"%s\" aplicado", name);
        } else if(said) {
          say("NeoHEN: aspecto de fabrica");
        }
        said = 1;
        snprintf(shown, sizeof(shown), "%s", sig);
        write_state(name, "ok");
      }
    }
    cover_menus();
    sleep(POLL_SECONDS);
  }
  return 0;
}
