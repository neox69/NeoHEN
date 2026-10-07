/* NeoHEN themes service.
 *
 * Keeps the theme named in /data/neohen/themes/active.txt (at start, the one
 * in default.txt if that file exists) shown on top of what the menu reads
 * from /system_ex: its assets folder (home music, backgrounds, the .rco
 * bundles) and any other folder the theme has files for, such as the icon
 * of a system app. An empty or missing active.txt means the stock look.
 *
 * A theme is a folder. The files directly in it go over
 * /system_ex/vsh_asset; a system_ex/ tree inside it mirrors the console's
 * own, one overlay per folder that has files:
 *
 *   My theme/bgm_home.at9
 *   My theme/bg_hub_default.dds.zst
 *   My theme/system_ex/app/NPXS40074/sce_sys/icon0.png
 *
 * (what is given for system_ex/rnps/ also goes over /mnt/rnps/, where the
 * console keeps the updated copy of the menu's own apps)
 *
 * How: each of those folders has a store here (themes/.current for the
 * assets, themes/.overlay/<path> for the others) that is put over the
 * menu's folder once, with a READ-ONLY unionfs mount (the mechanism
 * ShadowMount+ uses for its backport overlays), and is not taken off while
 * that menu lives: the menu keeps files of it open, and an unmount fails
 * while it does. Changing theme only changes what is inside the stores,
 * which hold hard links to the active theme's files; empty, the menu sees
 * the stock files.
 *
 * SceShellUI (NPXS40087) reads /system_ex through its own sandbox, a nullfs
 * view made when it starts, so the mounts go on that path and are made
 * again whenever the menu restarts (Rest Mode, a NeoHEN reload, "apply in
 * full"). A new menu is looked for ten times a second, to be there before
 * it reads anything.
 *
 * Two rules. Nothing here writes below /system or /system_ex: a mount lives
 * in the kernel's mount table, so a reboot always brings the stock files
 * back, untouched. And a theme that makes the menu die twice in a row right
 * after starting is taken off by itself.
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
#define DEFAULT  THEMES "/default.txt"
#define CURRENT  THEMES "/.current"
#define OVERLAY  THEMES "/.overlay"
#define SANDBOX  "/mnt/sandbox"
#define MENU     "NPXS40087_"
#define ASSETS   "system_ex/vsh_asset"
#define TREE     "system_ex"
#define RNPS_STOCK "system_ex/rnps/"
#define RNPS_LIVE  "mnt/rnps/"
#define SELF     "neotheme.elf"

#define TICK_MS        100 /* how often a new menu is looked for */
#define TICKS_PER_PASS 20  /* the theme itself is looked at every 2 s */
#define CRASH_SECONDS  20  /* a menu gone sooner than this did not start well */
#define MAX_LAYERS     96
#define MAX_DEPTH      8

/* One folder of the console covered by one store of ours. */
typedef struct layer {
  char rel[256];   /* path below the menu's sandbox, no leading slash */
  char store[400]; /* the folder of ours that goes over it */
} layer_t;

static layer_t g_layers[MAX_LAYERS];
static int g_nlayers;

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

/* ---------------------------------------------------------------- layers */

/* A path a theme may cover: below system_ex/, plain names only. */
static int
rel_ok(const char *rel) {
  if(strncmp(rel, TREE "/", strlen(TREE) + 1) || strstr(rel, "..") || strchr(rel, '~')) {
    return 0;
  }
  for(const char *p = rel; *p; p++) {
    if(!isalnum((unsigned char)*p) && !strchr("/._-", *p)) {
      return 0;
    }
  }
  return 1;
}

/* The layer for a path, made if it is new. NULL if it cannot be had. */
static layer_t *
layer_for(const char *rel) {
  layer_t *l;

  for(int i = 0; i < g_nlayers; i++) {
    if(!strcmp(g_layers[i].rel, rel)) {
      return &g_layers[i];
    }
  }
  if(g_nlayers == MAX_LAYERS || !rel_ok(rel) || strlen(rel) >= sizeof(l->rel)) {
    return NULL;
  }
  l = &g_layers[g_nlayers];
  snprintf(l->rel, sizeof(l->rel), "%s", rel);
  if(!strcmp(rel, ASSETS)) {
    snprintf(l->store, sizeof(l->store), "%s", CURRENT);
  } else {
    /* one flat folder per path: system_ex~app~NPXS40074~sce_sys */
    size_t n = (size_t)snprintf(l->store, sizeof(l->store), OVERLAY "/%s", rel);
    if(n >= sizeof(l->store)) {
      return NULL;
    }
    for(char *p = l->store + strlen(OVERLAY) + 1; *p; p++) {
      if(*p == '/') {
        *p = '~';
      }
    }
  }
  mkdir(OVERLAY, 0777);
  mkdir(l->store, 0777);
  g_nlayers++;
  return l;
}

/* The stores a previous run left: they may still be mounted and hold the
 * files of an old theme, so they are layers from the start. */
static void
load_layers(void) {
  struct dirent *e;
  DIR *dir;

  layer_for(ASSETS);
  if(!(dir = opendir(OVERLAY))) {
    return;
  }
  while((e = readdir(dir))) {
    char rel[256];
    if(e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(rel)) {
      continue;
    }
    snprintf(rel, sizeof(rel), "%s", e->d_name);
    for(char *p = rel; *p; p++) {
      if(*p == '~') {
        *p = '/';
      }
    }
    layer_for(rel);
  }
  closedir(dir);
}

static int
store_empty(const char *store) {
  struct dirent *e;
  int empty = 1;
  DIR *dir = opendir(store);

  if(!dir) {
    return 1;
  }
  while((e = readdir(dir))) {
    if(strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
      empty = 0;
      break;
    }
  }
  closedir(dir);
  return empty;
}

static void
empty_store(const char *store) {
  struct dirent *e;
  char path[768];
  DIR *dir = opendir(store);

  if(!dir) {
    return;
  }
  while((e = readdir(dir))) {
    if(strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
      snprintf(path, sizeof(path), "%s/%s", store, e->d_name);
      unlink(path);
    }
  }
  closedir(dir);
}

/* ---------------------------------------------------------------- mounts */

/* Is this store already on this folder? */
static int
mounted(const char *target, const char *store) {
  struct statfs sfs;

  if(statfs(target, &sfs) || strcmp(sfs.f_fstypename, "unionfs") ||
     strcmp(sfs.f_mntonname, target)) {
    return 0;
  }
  const char *from = sfs.f_mntfromname;
  if(!strncmp(from, "<above>:", 8)) {
    from += 8;
  }
  return !strcmp(from, store);
}

#define IOV(s) {(void *)(s), (s) ? strlen(s) + 1 : 0}

static int
cover(const char *target, const char *store) {
  struct iovec iov[] = {
      IOV("fstype"),   IOV("unionfs"),
      IOV("from"),     IOV(store),
      IOV("fspath"),   IOV(target),
      IOV("copymode"), IOV("transparent"),
      IOV("noatime"),  IOV(NULL),
      IOV("fnodup"),   IOV(NULL),
  };
  /* MNT_RDONLY: nothing can be written through the mount, in either layer */
  return nmount(iov, sizeof(iov) / sizeof(iov[0]), MNT_RDONLY);
}

/* A menu that is running has its own program mounted in its sandbox; what
 * is left of a menu that is gone does not. */
static int
menu_alive(const char *sandbox) {
  char path[512];
  struct stat st;

  snprintf(path, sizeof(path), SANDBOX "/%s/app0/eboot.bin", sandbox);
  return !stat(path, &st);
}

/* The sandbox of the menu that is running, "" if there is none. */
static void
live_menu(char *out, size_t size) {
  struct dirent *e;
  DIR *dir = opendir(SANDBOX);

  out[0] = 0;
  if(!dir) {
    return;
  }
  while((e = readdir(dir))) {
    if(!strncmp(e->d_name, MENU, strlen(MENU)) && menu_alive(e->d_name)) {
      snprintf(out, size, "%s", e->d_name);
    }
  }
  closedir(dir);
}

/* Puts every store that is needed over its folder in every live menu, and
 * takes ours off the folders a dead menu left behind (nothing holds them
 * any more). Returns how many mounts could not be made. */
static int
cover_menus(void) {
  struct dirent *e;
  int failed = 0;
  DIR *dir = opendir(SANDBOX);

  if(!dir) {
    return 1;
  }
  while((e = readdir(dir))) {
    int alive;

    if(strncmp(e->d_name, MENU, strlen(MENU))) {
      continue;
    }
    alive = menu_alive(e->d_name);
    for(int i = 0; i < g_nlayers; i++) {
      /* the menu's own apps are read from /mnt/rnps when the console has a
       * newer copy of them there, so that folder gets the same store */
      const char *rel = g_layers[i].rel;
      int twice = !strncmp(rel, RNPS_STOCK, strlen(RNPS_STOCK));

      for(int k = 0; k <= twice; k++) {
        char target[768];
        struct stat st;

        if(k) {
          snprintf(target, sizeof(target), SANDBOX "/%s/" RNPS_LIVE "%s", e->d_name,
                   rel + strlen(RNPS_STOCK));
        } else {
          snprintf(target, sizeof(target), SANDBOX "/%s/%s", e->d_name, rel);
        }
        if(stat(target, &st)) {
          continue;
        }
        if(mounted(target, g_layers[i].store)) {
          if(!alive) {
            unmount(target, 0); /* not forced: it stays if something uses it */
          }
          continue;
        }
        /* the assets are always covered, so a theme can come later without
         * a menu restart; the rest only when the theme has files for them */
        if(alive && (i == 0 || !store_empty(g_layers[i].store)) &&
           cover(target, g_layers[i].store)) {
          failed++;
        }
      }
    }
  }
  closedir(dir);
  return failed;
}

/* Has this menu got the assets store yet? Its /system_ex shows up a moment
 * after the menu itself does. */
static int
assets_covered(const char *sandbox) {
  char target[512];

  snprintf(target, sizeof(target), SANDBOX "/%s/" ASSETS, sandbox);
  return mounted(target, CURRENT);
}

/* ----------------------------------------------------------------- theme */

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

/* A theme's .rco bundle only fits the firmware it was made from: one made
 * with themes/tools/rco.py keeps the original's header but for the size of
 * its file table, the last of its numbers. Any other is left out, and the
 * menu keeps its own. */
#define RCO_SAME_BYTES 76

static int
rco_fits(const char *path, const char *name) {
  unsigned char mine[RCO_SAME_BYTES], stock[RCO_SAME_BYTES];
  char original[512];
  int ok = 0;
  FILE *a, *b;

  snprintf(original, sizeof(original), "/" ASSETS "/%s", name);
  a = fopen(path, "rb");
  b = fopen(original, "rb");
  if(a && b && fread(mine, 1, sizeof(mine), a) == sizeof(mine) &&
     fread(stock, 1, sizeof(stock), b) == sizeof(stock)) {
    ok = !memcmp(mine, stock, sizeof(mine));
  }
  if(a) {
    fclose(a);
  }
  if(b) {
    fclose(b);
  }
  return ok;
}

typedef struct tally {
  unsigned long long bytes;
  long newest;
  int files;
  int failed;
} tally_t;

/* Goes through one folder of a theme, whose files belong over `rel`, and
 * through the folders below it. With `fill`, links them into the stores;
 * always counts them. `dir` is the theme's folder for it. */
static void
walk(const char *dir_path, const char *rel, int depth, int fill, tally_t *t) {
  struct dirent *e;
  layer_t *layer = NULL;
  DIR *dir = opendir(dir_path);

  if(!dir) {
    return;
  }
  while((e = readdir(dir))) {
    char path[768], sub[256];
    struct stat st;

    if(skipped(e->d_name)) {
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", dir_path, e->d_name);
    if(stat(path, &st)) {
      continue;
    }
    if(S_ISDIR(st.st_mode)) {
      /* only the system_ex tree of a theme is followed */
      if(rel[0]) {
        snprintf(sub, sizeof(sub), "%s/%s", rel, e->d_name);
      } else if(!strcmp(e->d_name, TREE)) {
        snprintf(sub, sizeof(sub), TREE);
      } else {
        continue;
      }
      if(depth < MAX_DEPTH) {
        walk(path, sub, depth + 1, fill, t);
      }
      continue;
    }
    if(!S_ISREG(st.st_mode) || !strcmp(rel, TREE)) {
      continue; /* nothing goes over system_ex itself */
    }
    t->files++;
    t->bytes += (unsigned long long)st.st_size;
    if((long)st.st_mtime > t->newest) {
      t->newest = (long)st.st_mtime;
    }
    if(!fill) {
      continue;
    }
    if(!layer && !(layer = layer_for(rel[0] ? rel : ASSETS))) {
      t->failed++;
      continue;
    }
    size_t len = strlen(e->d_name);
    if(len > 4 && !strcmp(e->d_name + len - 4, ".rco") && !strcmp(layer->rel, ASSETS) &&
       !rco_fits(path, e->d_name)) {
      t->failed++;
      continue;
    }
    char to[768];
    snprintf(to, sizeof(to), "%s/%s", layer->store, e->d_name);
    unlink(to); /* given twice (root and system_ex/vsh_asset): one stays */
    if(link(path, to) && copy_file(path, to)) {
      t->failed++;
    }
  }
  closedir(dir);
}

/* Makes the stores hold exactly the files of the theme ("" = none). Files
 * are hard links, so nothing is copied and a theme takes no extra space. */
static int
fill_stores(const char *theme) {
  tally_t t = {0};

  for(int i = 0; i < g_nlayers; i++) {
    empty_store(g_layers[i].store);
  }
  if(theme[0]) {
    walk(theme, "", 0, 1, &t);
  }
  return t.failed ? -1 : 0;
}

/* Changes when the theme changes, and when a file of it is added, removed
 * or written again (a new music file uploaded over the old one). */
static void
signature(const char *theme, char *out, size_t size) {
  tally_t t = {0};

  if(!theme[0]) {
    snprintf(out, size, "-");
    return;
  }
  walk(theme, "", 0, 0, &t);
  snprintf(out, size, "%s|%d|%llu|%ld", theme, t.files, t.bytes, t.newest);
}

int
main(void) {
  char shown[640] = "";  /* signature of what the stores hold */
  char menu[64] = "";    /* the live menu's sandbox */
  time_t menu_since = 0; /* when it was first seen */
  int short_lived = 0;   /* menus in a row that died right after starting */
  int uncovered = 0;     /* the live menu has not got the stores yet */
  int said = 0;

  kill_previous();
  syscall(SYS_thr_set_name, -1, SELF);
  mkdir(THEMES, 0777);
  mkdir(CURRENT, 0777);
  load_layers();

  /* themes/default.txt names the theme every start begins with, whatever
   * was in use before; without it the last choice stays. */
  FILE *def = fopen(DEFAULT, "r");
  if(def) {
    char line[128] = "";
    if(fgets(line, sizeof(line), def)) {
      FILE *act = fopen(ACTIVE, "w");
      if(act) {
        fputs(line, act);
        fclose(act);
      }
    }
    fclose(def);
  }

  for(unsigned tick = 0;; tick++) {
    char now[64];

    live_menu(now, sizeof(now));
    if(now[0] && strcmp(now, menu)) {
      /* a new menu: cover it before it reads anything */
      time_t t = time(NULL);
      if(menu[0] && shown[0] && strcmp(shown, "-")) {
        short_lived = t - menu_since < CRASH_SECONDS ? short_lived + 1 : 0;
        if(short_lived >= 2) {
          /* the menu keeps dying with this theme on: take it off */
          FILE *act = fopen(ACTIVE, "w");
          if(act) {
            fclose(act);
          }
          unlink(DEFAULT); /* or the next start would put it back */
          fill_stores("");
          snprintf(shown, sizeof(shown), "-");
          write_state("", "reverted");
          say("NeoHEN: el menu fallaba con el tema, vuelve el aspecto de fabrica");
          short_lived = 0;
        }
      }
      snprintf(menu, sizeof(menu), "%s", now);
      menu_since = t;
      uncovered = 1;
    }
    if(uncovered) {
      cover_menus();
      uncovered = !assets_covered(menu);
    }

    if(tick % TICKS_PER_PASS == 0) {
      char theme[512], name[128], sig[640];

      read_active(theme, sizeof(theme), name, sizeof(name));
      signature(theme, sig, sizeof(sig));
      if(strcmp(sig, shown)) {
        if(fill_stores(theme)) {
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
    }
    usleep(TICK_MS * 1000);
  }
  return 0;
}
