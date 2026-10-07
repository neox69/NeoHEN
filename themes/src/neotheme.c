/* NeoHEN themes - proof of concept.
 *
 * Shows the files of /data/neohen/themes/_active on top of the menu's view
 * of /system_ex/vsh_asset (home music, backgrounds) with a READ-ONLY unionfs
 * mount, the mechanism ShadowMount+ uses for its backport overlays. Sending
 * the payload again removes the mount.
 *
 * The one rule: nothing here writes below /system or /system_ex. A mount
 * lives in the kernel's mount table, so a reboot always brings the stock
 * files back, untouched.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define TARGET "/system_ex/vsh_asset"
#define THEME  "/data/neohen/themes/_active"
#define LOGF   "/data/neohen/themes/neotheme.log"

typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void
say(const char *fmt, ...) {
  notify_request_t req;
  va_list ap;
  FILE *f;

  memset(&req, 0, sizeof(req));
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof(req.message), fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
  if((f = fopen(LOGF, "a"))) {
    fprintf(f, "%ld %s\n", (long)time(NULL), req.message);
    fclose(f);
  }
}

#define IOV(s) {(void *)(s), (s) ? strlen(s) + 1 : 0}

/* Puts the theme over one folder, or takes it off if it is already there.
 * Returns 1 = put, 0 = taken off, -1 = error (already reported). */
static int
toggle(const char *target) {
  struct statfs sfs;

  if(statfs(target, &sfs)) {
    /* the sandbox of a SceShellUI that is gone: nothing to do, nothing to say */
    return -1;
  }
  if(!strcmp(sfs.f_fstypename, "unionfs") && !strcmp(sfs.f_mntonname, target)) {
    if(unmount(target, 0)) {
      say("NeoHEN temas: no se pudo quitar el tema de %s (%s). Reinicia la "
          "consola para volver al aspecto de fabrica", target, strerror(errno));
      return -1;
    }
    return 0;
  }

  struct iovec iov[] = {
      IOV("fstype"),   IOV("unionfs"),
      IOV("from"),     IOV(THEME),
      IOV("fspath"),   IOV(target),
      IOV("copymode"), IOV("transparent"),
      IOV("noatime"),  IOV(NULL),
      IOV("fnodup"),   IOV(NULL),
  };
  /* MNT_RDONLY: nothing can be written through the mount, in either layer */
  if(nmount(iov, sizeof(iov) / sizeof(iov[0]), MNT_RDONLY)) {
    say("NeoHEN temas: el montaje sobre %s ha fallado (%s). No se ha cambiado "
        "nada", target, strerror(errno));
    return -1;
  }
  return 1;
}

int
main(void) {
  struct stat st;
  struct dirent *e;
  DIR *dir;
  int put = 0, off = 0;

  if(stat(THEME, &st) || !S_ISDIR(st.st_mode)) {
    say("NeoHEN temas: no existe la carpeta %s", THEME);
    return 1;
  }

  /* An overlay left on the global folder by the first test build. */
  struct statfs sfs;
  if(!statfs(TARGET, &sfs) && !strcmp(sfs.f_fstypename, "unionfs") &&
     !strcmp(sfs.f_mntonname, TARGET)) {
    unmount(TARGET, 0);
  }

  /* SceShellUI (NPXS40087) reads its assets through its own sandbox, a
   * nullfs view of /system_ex made when it started: a mount on the global
   * folder is not seen through it, so the theme goes on the sandbox path. */
  if(!(dir = opendir("/mnt/sandbox"))) {
    say("NeoHEN temas: no se puede abrir /mnt/sandbox (%s)", strerror(errno));
    return 1;
  }
  while((e = readdir(dir))) {
    char target[512];
    if(strncmp(e->d_name, "NPXS40087_", 10)) {
      continue;
    }
    snprintf(target, sizeof(target), "/mnt/sandbox/%s/system_ex/vsh_asset", e->d_name);
    int r = toggle(target);
    put += r == 1;
    off += r == 0;
  }
  closedir(dir);

  if(put) {
    say("NeoHEN temas: tema puesto en el menu (solo lectura)");
  } else if(off) {
    say("NeoHEN temas: tema quitado, aspecto de fabrica");
  }
  return 0;
}
