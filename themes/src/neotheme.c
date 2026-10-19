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
 *   My theme/background.dds.zst      one image for every background
 *   My theme/sounds/snd_enter.vag    a menu sound, by its name
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
 * full"). A new menu is looked for forty times a second, to be there before
 * it reads anything: its sounds and icons are read once, as it starts.
 *
 * Menu sounds live inside one of the console's .rco bundles, which only
 * fits its firmware: a theme brings the sounds alone and the bundle is made
 * here, from the console's own, with the new sounds added at its end.
 *
 * The themes that come with NeoHEN are packed inside this program and are
 * written to the themes folder the first time, if they are not there; on a
 * console with no theme chosen yet, the first of them is put on.
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>

#define THEMES   "/data/neohen/themes"
#define ACTIVE   THEMES "/active.txt"
#define STATE    THEMES "/state.txt"
#define LOGFILE  THEMES "/neotheme.log"
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
#define BUNDLED  THEMES "/.bundled"
#define ONE_BG   "background.dds.zst"
#define SOUNDS   "sounds"
#define ICONS    "icons"
#define UI_RCO   "Sce.PlayStation.PUI_UI3.rco"
#define MAX_SOUNDS 48
#define MAX_ICONS  64
#define MAX_SVG    (16 << 10)

/* every background the menu has; a theme's background.dds.zst is all of them */
static const char *const BG_NAMES[] = {
  "bg_hub_default", "Library_BG_blue_Low_Angle_001", "bg_NPXS40037", "bg_NPXS40047",
  "bg_NPXS40053", "bg_NPXS40054", "bg_NPXS40056", "bg_NPXS40063", "bg_NPXS40071",
  "bg_NPXS40075", "bg_NPXS40080", "bg_NPXS40110", "bg_NPXS40139", "bg_NPXS40144",
  "bg_NPXS40154",
};
#define NBG ((int)(sizeof(BG_NAMES) / sizeof(BG_NAMES[0])))

/* the themes packed into this program (themes/tools/pack_themes.py) */
extern const unsigned char theme_pack[];
extern const unsigned char theme_pack_end[];

#define TICK_MS        25 /* how often a new menu is looked for */
#define TICKS_PER_PASS 80 /* the theme itself is looked at every 2 s */
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

/* A short diary, started again each run. */
static void
note(const char *fmt, ...) {
  va_list ap;
  FILE *f = fopen(LOGFILE, "a");

  if(!f) {
    return;
  }
  fprintf(f, "%ld ", (long)time(NULL));
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}

/* How long ago the menu's process started, in ms; -1 if it is not found. */
static long
menu_age_ms(void) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  long age = -1;
  struct timeval now;
  char *buf;

  if(sysctl(mib, 4, NULL, &size, NULL, 0)) {
    return -1;
  }
  size += 16 * sizeof(struct kinfo_proc);
  if(!(buf = malloc(size)) || sysctl(mib, 4, buf, &size, NULL, 0)) {
    free(buf);
    return -1;
  }
  gettimeofday(&now, NULL);
  for(char *ptr = buf; ptr < buf + size;) {
    struct kinfo_proc *ki = (struct kinfo_proc *)ptr;
    if(ki->ki_structsize <= 0) {
      break;
    }
    ptr += ki->ki_structsize;
    if(!strcmp(ki->ki_comm, "SceShellUI")) {
      age = (now.tv_sec - ki->ki_start.tv_sec) * 1000L +
            (now.tv_usec - ki->ki_start.tv_usec) / 1000L;
    }
  }
  free(buf);
  return age;
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

/* The sandbox of the newest menu that is running, "" if there is none. The
 * one before it stays for a moment after it is killed, and names only go
 * up, so the newest is the highest. */
static void
live_menu(char *out, size_t size) {
  struct dirent *e;
  DIR *dir = opendir(SANDBOX);

  out[0] = 0;
  if(!dir) {
    return;
  }
  while((e = readdir(dir))) {
    if(!strncmp(e->d_name, MENU, strlen(MENU)) && strcmp(e->d_name, out) > 0 &&
       menu_alive(e->d_name)) {
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
    if(!rel[0] && !strcmp(e->d_name, ONE_BG)) {
      /* under every background's name the theme has no file of its own for */
      for(int i = 0; i < NBG; i++) {
        char own[768], as[768];
        struct stat have;
        snprintf(own, sizeof(own), "%s/%s.dds.zst", dir_path, BG_NAMES[i]);
        if(!stat(own, &have)) {
          continue;
        }
        snprintf(as, sizeof(as), "%s/%s.dds.zst", layer->store, BG_NAMES[i]);
        unlink(as);
        if(link(path, as) && copy_file(path, as)) {
          t->failed++;
        }
      }
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

/* ---------------------------------------------------------------- sounds
 * An .rco is a tree of elements, each with attributes, and a table of
 * files at its end. A sound is an element <sounddata id="snd_enter"
 * src=(offset, size)>. The copy made here is the console's bundle with each
 * new sound added after the last file and that one (offset, size) changed:
 * nothing else moves. */

static uint32_t
rd32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
wr32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16);
  p[3] = (unsigned char)(v >> 24);
}

typedef struct rco {
  unsigned char *head; /* the bundle up to its table of files */
  uint32_t head_size;
  uint32_t tree_off, tree_size, id_off, id_size, str_off, str_size;
} rco_t;

static const char *
rco_text(const rco_t *r, uint32_t table, uint32_t table_size, uint32_t off) {
  if(off >= table_size || !memchr(r->head + table + off, 0, table_size - off)) {
    return "";
  }
  return (const char *)r->head + table + off;
}

/* Where the (offset, size) of the file of the element called `ident` is
 * kept, as a position in the bundle; 0 if there is no such element. */
static uint32_t
rco_find(const rco_t *r, const char *ident, uint32_t off, int depth) {
  while(off != 0xffffffffu && depth < 32) {
    uint32_t base = r->tree_off + off, nattr, next, first, src = 0;
    int mine = 0;

    if(off + 28 > r->tree_size) {
      return 0;
    }
    nattr = rd32(r->head + base + 4);
    next = rd32(r->head + base + 16);
    first = rd32(r->head + base + 20);
    if(nattr > 64 || off + 28 + nattr * 16 > r->tree_size) {
      return 0;
    }
    for(uint32_t i = 0; i < nattr; i++) {
      const unsigned char *a = r->head + base + 28 + i * 16;
      const char *name = rco_text(r, r->str_off, r->str_size, rd32(a));
      uint32_t type = rd32(a + 4), v1 = rd32(a + 8);

      if(type == 8 && !strcmp(name, "src")) {
        src = base + 28 + i * 16 + 8;
      } else if((type == 9 || type == 10) && !strcmp(name, "id")) {
        mine = !strcmp(rco_text(r, r->id_off, r->id_size, v1 + 4), ident);
      }
    }
    if(mine && src) {
      return src;
    }
    if(first != 0xffffffffu) {
      uint32_t deeper = rco_find(r, ident, first, depth + 1);
      if(deeper) {
        return deeper;
      }
    }
    off = next;
  }
  return 0;
}

/* The theme's sounds, as "snd_enter" names (no folder, no .vag). */
static int
list_sounds(const char *theme, char names[][64], int max, tally_t *t) {
  char dir_path[768];
  struct dirent *e;
  int n = 0;
  DIR *dir;

  snprintf(dir_path, sizeof(dir_path), "%s/" SOUNDS, theme);
  if(!(dir = opendir(dir_path))) {
    return 0;
  }
  while((e = readdir(dir)) && n < max) {
    char path[1024];
    struct stat st;
    size_t len = strlen(e->d_name);

    if(len < 5 || len >= 64 || strcmp(e->d_name + len - 4, ".vag")) {
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", dir_path, e->d_name);
    if(stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 64 || st.st_size > (4 << 20)) {
      continue;
    }
    if(t) {
      t->files++;
      t->bytes += (unsigned long long)st.st_size;
      if((long)st.st_mtime > t->newest) {
        t->newest = (long)st.st_mtime;
      }
    }
    snprintf(names[n], 64, "%.*s", (int)(len - 4), e->d_name);
    n++;
  }
  closedir(dir);
  return n;
}

/* The theme's menu icons, as "iconid_home" names: icons/<name>.svg, the
 * form ProsperoThemes' packs (.p5theme) bring them in. */
static int
list_icons(const char *theme, char names[][64], int max, tally_t *t) {
  char dir_path[768];
  struct dirent *e;
  int n = 0;
  DIR *dir;

  snprintf(dir_path, sizeof(dir_path), "%s/" ICONS, theme);
  if(!(dir = opendir(dir_path))) {
    return 0;
  }
  while((e = readdir(dir)) && n < max) {
    char path[1024];
    struct stat st;
    size_t len = strlen(e->d_name);

    if(len < 5 || len >= 64 || strcmp(e->d_name + len - 4, ".svg")) {
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", dir_path, e->d_name);
    if(stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 16 || st.st_size > MAX_SVG) {
      continue;
    }
    if(t) {
      t->files++;
      t->bytes += (unsigned long long)st.st_size;
      if((long)st.st_mtime > t->newest) {
        t->newest = (long)st.st_mtime;
      }
    }
    snprintf(names[n], 64, "%.*s", (int)(len - 4), e->d_name);
    n++;
  }
  closedir(dir);
  return n;
}

static const unsigned char *
find(const unsigned char *hay, size_t n, const char *needle) {
  size_t k = strlen(needle);
  const unsigned char *p = hay, *last;

  if(n < k) {
    return NULL;
  }
  last = hay + n - k;
  while(p <= last && (p = memchr(p, needle[0], (size_t)(last - p) + 1)) != NULL) {
    if(!memcmp(p, needle, k)) {
      return p;
    }
    p++;
  }
  return NULL;
}

/* An icon's SVG, whitespace runs made one space; its length, or -1. */
static long
svg_read(const char *path, char *out, size_t size) {
  FILE *f = fopen(path, "rb");
  size_t n = 0;
  int c, space = 0;

  if(!f) {
    return -1;
  }
  while((c = fgetc(f)) != EOF && n < size) {
    if(c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      space = n > 0;
      continue;
    }
    if(space && c != '<' && n && out[n - 1] != '>') {
      out[n++] = ' ';
    }
    space = 0;
    if(n < size) {
      out[n++] = (char)c;
    }
  }
  fclose(f);
  if(c != EOF || n < 6 || memcmp(out + n - 6, "</svg>", 6)) {
    return -1;
  }
  return (long)n;
}

/* Puts the theme's icons into the bundle at fd: the menu keeps its icons as
 * plain SVG text, most named by a <title>group/iconid_name</title>. Each one
 * replaced is padded with spaces to the original's length, so nothing in
 * the bundle moves (as ProsperoThemes does). Returns how many went in. */
static int
put_icons(int fd, const char *theme, char names[][64], int count) {
  static char svg[MAX_SVG];
  struct stat st;
  unsigned char *map, *p, *end;
  int put = 0;

  if(fstat(fd, &st) || st.st_size <= 0) {
    return 0;
  }
  map = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if(map == MAP_FAILED) {
    note("icons: the bundle could not be mapped: %s", strerror(errno));
    return 0;
  }
  end = map + st.st_size;
  p = map;
  for(int idx = 0; (p = (unsigned char *)find(p, (size_t)(end - p), "<svg")) != NULL; idx++) {
    const unsigned char *close = find(p, (size_t)(end - p), "</svg>"), *t, *te, *name = NULL;
    size_t len, nlen = 0;
    if(!close) {
      break;
    }
    close += 6;
    len = (size_t)(close - p);
    t = find(p, len, "<title>");
    te = t ? find(t, (size_t)(close - t), "</title>") : NULL;
    if(t && te) {
      name = te;
      t += 7;
      while(name > t && name[-1] != '/') {
        name--;
      }
      nlen = (size_t)(te - name);
    } else if(!t && idx == 875 && len == 512) {
      /* the top bar's search magnifier has no title: the 876th icon, as
       * on 13.00 and 13.42 */
      name = (const unsigned char *)"iconid_search";
      nlen = 13;
    }
    if(name) {
      for(int i = 0; i < count; i++) {
        char path[1024];
        long n;
        if(strlen(names[i]) != nlen || memcmp(name, names[i], nlen)) {
          continue;
        }
        snprintf(path, sizeof(path), "%s/" ICONS "/%s.svg", theme, names[i]);
        if((n = svg_read(path, svg, sizeof(svg))) < 0 || (size_t)n > len) {
          note("icons: %s %s", names[i], n < 0 ? "is not an SVG" : "is longer than the console's");
          break;
        }
        memcpy(p, svg, (size_t)n - 6);
        memset(p + n - 6, ' ', len - (size_t)n);
        memcpy(p + len - 6, "</svg>", 6);
        put++;
        break;
      }
    }
    p = (unsigned char *)close;
  }
  munmap(map, (size_t)st.st_size);
  return put;
}

/* Adds one sound to the copy being written and points the element at it. */
static int
rco_put(rco_t *r, FILE *out, uint32_t file_off, const char *ident, const char *vag_path) {
  static const unsigned char zeros[16];
  unsigned char buf[65536];
  uint32_t src = rco_find(r, ident, 0, 0);
  long at;
  size_t n, total = 0;
  FILE *in;

  if(!src) {
    return 1; /* this firmware has no such sound: not an error */
  }
  if(!(in = fopen(vag_path, "rb"))) {
    return -1;
  }
  if(fread(buf, 1, 4, in) != 4 || memcmp(buf, "VAGp", 4)) {
    fclose(in);
    return 1; /* not a sound of the kind the menu plays */
  }
  rewind(in);
  fseek(out, 0, SEEK_END);
  at = ftell(out);
  if(at % 16) {
    fwrite(zeros, 1, (size_t)(16 - at % 16), out);
    at += 16 - at % 16;
  }
  while((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if(fwrite(buf, 1, n, out) != n) {
      fclose(in);
      return -1;
    }
    total += n;
  }
  fclose(in);
  wr32(r->head + src, (uint32_t)at - file_off);
  wr32(r->head + src + 4, (uint32_t)total);
  return 0;
}

/* Makes the assets store's copy of the menu's bundle with the theme's
 * sounds in it. 0 if it is there (or the theme has none), -1 if not. */
static int
build_sounds(const char *theme) {
  static const char *const ALSO[] = {"snd_focus_move", "snd_enter", "snd_cancel"};
  char names[MAX_SOUNDS][64], tmp[512], final[512], vag[1024];
  unsigned char hdr[80];
  rco_t r = {0};
  uint32_t file_off, file_size;
  static char inames[MAX_ICONS][64];
  int count = list_sounds(theme, names, MAX_SOUNDS, NULL), put = 0, bad = 0;
  int icount = list_icons(theme, inames, MAX_ICONS, NULL), iput = 0;
  struct stat st;
  long end;
  FILE *in, *out;

  snprintf(final, sizeof(final), CURRENT "/" UI_RCO);
  if((!count && !icount) || !stat(final, &st)) {
    return 0; /* no sounds or icons, or the theme brings a whole bundle of its own */
  }
  if(!(in = fopen("/" ASSETS "/" UI_RCO, "rb"))) {
    return -1;
  }
  if(fread(hdr, 1, sizeof(hdr), in) != sizeof(hdr) || memcmp(hdr, "RCOF", 4) || fstat(fileno(in), &st)) {
    fclose(in);
    return -1;
  }
  r.tree_off = rd32(hdr + 8);
  r.tree_size = rd32(hdr + 12);
  r.id_off = rd32(hdr + 16);
  r.id_size = rd32(hdr + 20);
  r.str_off = rd32(hdr + 32);
  r.str_size = rd32(hdr + 36);
  file_off = rd32(hdr + 72);
  file_size = rd32(hdr + 76);
  /* the layout this was written for: the files last, everything else before */
  if((uint64_t)file_off + file_size != (uint64_t)st.st_size || file_off > (8u << 20) ||
     (uint64_t)r.tree_off + r.tree_size > file_off || (uint64_t)r.id_off + r.id_size > file_off ||
     (uint64_t)r.str_off + r.str_size > file_off || !(r.head = malloc(file_off))) {
    fclose(in);
    return -1;
  }
  r.head_size = file_off;
  rewind(in);
  snprintf(tmp, sizeof(tmp), CURRENT "/." UI_RCO ".new");
  if(fread(r.head, 1, file_off, in) != file_off || !(out = fopen(tmp, "w+b"))) {
    free(r.head);
    fclose(in);
    return -1;
  }
  /* the whole original first */
  rewind(in);
  {
    static unsigned char buf[1 << 20];
    size_t n;
    while((n = fread(buf, 1, sizeof(buf), in)) > 0) {
      if(fwrite(buf, 1, n, out) != n) {
        bad = 1;
        break;
      }
    }
  }
  fclose(in);

  for(int i = 0; i < count && !bad; i++) {
    int got;
    snprintf(vag, sizeof(vag), "%s/" SOUNDS "/%s.vag", theme, names[i]);
    got = rco_put(&r, out, file_off, names[i], vag);
    bad = got < 0;
    put += got == 0;
    /* the console keeps other sets of these three (snd_1_enter...): they get
     * the same sound unless the theme gives one for them */
    for(size_t k = 0; k < sizeof(ALSO) / sizeof(ALSO[0]) && !bad; k++) {
      if(strcmp(names[i], ALSO[k])) {
        continue;
      }
      for(int set = 1; set <= 4 && !bad; set++) {
        char alt[80], own[1024];
        struct stat have;
        snprintf(alt, sizeof(alt), "snd_%d_%s", set, ALSO[k] + 4);
        snprintf(own, sizeof(own), "%s/" SOUNDS "/%s.vag", theme, alt);
        if(stat(own, &have)) {
          bad = rco_put(&r, out, file_off, alt, vag) < 0;
        }
      }
    }
  }
  if(!bad) {
    static const unsigned char zeros[16];
    fseek(out, 0, SEEK_END);
    end = ftell(out);
    if(end % 16) {
      fwrite(zeros, 1, (size_t)(16 - end % 16), out);
      end += 16 - end % 16;
    }
    wr32(r.head + 76, (uint32_t)end - file_off);
    rewind(out);
    bad = fwrite(r.head, 1, file_off, out) != file_off;
  }
  free(r.head);
  if(!bad && icount) {
    bad = fflush(out) != 0;
    if(!bad) {
      iput = put_icons(fileno(out), theme, inames, icount);
    }
  }
  if(fclose(out) || bad || (!put && !iput) || rename(tmp, final)) {
    unlink(tmp);
    return put || iput || bad ? -1 : 0;
  }
  note("sounds: %d of the theme's %d, icons: %d of %d, put into the menu's bundle", put, count,
       iput, icount);
  return 0;
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
    if(build_sounds(theme)) {
      note("sounds: the menu's bundle could not be made; the menu keeps its own");
    }
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
  {
    char names[MAX_SOUNDS][64];
    static char inames[MAX_ICONS][64];
    list_sounds(theme, names, MAX_SOUNDS, &t);
    list_icons(theme, inames, MAX_ICONS, &t);
  }
  snprintf(out, size, "%s|%d|%llu|%ld", theme, t.files, t.bytes, t.newest);
}

/* ------------------------------------------------- the themes that come
 * Packed as: "NTPK", a number that changes with the pack, then for each
 * file its path's length (2 bytes), its size (4), the path ("Theme/...")
 * and the bytes; a zero length ends it. A theme is written only if its
 * folder is not there, and only once per pack: one the user deletes stays
 * deleted. */
static void
mkparents(char *path) {
  for(char *p = path + 1; *p; p++) {
    if(*p == '/') {
      *p = 0;
      mkdir(path, 0777);
      *p = '/';
    }
  }
}

/* The "version" number in a theme.json, or 0. */
static int
theme_version(const char *text, size_t len) {
  const char *k = len ? (const char *)find((const unsigned char *)text, len, "\"version\"") : NULL;
  if(!k) {
    return 0;
  }
  k += 9;
  while(k < text + len && (*k == ' ' || *k == ':')) {
    k++;
  }
  return k < text + len ? atoi(k) : 0;
}

/* The version of a theme already in the themes folder. */
static int
installed_version(const char *name) {
  char path[768], buf[512];
  size_t n = 0;
  FILE *f;

  snprintf(path, sizeof(path), THEMES "/%s/theme.json", name);
  if((f = fopen(path, "r"))) {
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
  }
  buf[n] = 0;
  return theme_version(buf, n);
}

/* The version of a theme in the pack, from its theme.json there. */
static int
packed_version(const char *name) {
  const unsigned char *p = theme_pack + 8, *end = theme_pack_end;
  size_t nlen = strlen(name);

  while(end - p >= 6) {
    uint32_t plen = (uint32_t)p[0] | ((uint32_t)p[1] << 8), size = rd32(p + 2);
    if(!plen || end - p < 6 + (long)plen + (long)size) {
      break;
    }
    if(plen == nlen + 11 && !memcmp(p + 6, name, nlen) && !memcmp(p + 6 + nlen, "/theme.json", 11)) {
      return theme_version((const char *)p + 6 + plen, size);
    }
    p += 6 + plen + size;
  }
  return 0;
}

static void
unpack_themes(void) {
  const unsigned char *p = theme_pack, *end = theme_pack_end;
  char id[16], had[16] = "", skip[16][64], take[16][64];
  int nskip = 0, ntake = 0, files = 0;
  FILE *f;

  if(end - p < 10 || memcmp(p, "NTPK", 4)) {
    return;
  }
  snprintf(id, sizeof(id), "%08x", rd32(p + 4));
  if((f = fopen(BUNDLED, "r"))) {
    if(!fgets(had, sizeof(had), f)) {
      had[0] = 0;
    }
    fclose(f);
    had[strcspn(had, "\r\n")] = 0;
  }
  if(!strcmp(had, id)) {
    return;
  }
  for(p += 8; end - p >= 2;) {
    uint32_t plen = (uint32_t)p[0] | ((uint32_t)p[1] << 8), size;
    char rel[400], path[768], name[64];
    const char *slash;
    int known = 0, wanted = 0;

    if(!plen || plen >= sizeof(rel) || end - p < 6 + (long)plen) {
      break;
    }
    size = rd32(p + 2);
    memcpy(rel, p + 6, plen);
    rel[plen] = 0;
    p += 6 + plen;
    if((uint64_t)(end - p) < size) {
      break;
    }
    slash = strchr(rel, '/');
    if(!slash || slash - rel >= (long)sizeof(name) || strstr(rel, "..") || rel[0] == '.') {
      p += size;
      continue;
    }
    snprintf(name, sizeof(name), "%.*s", (int)(slash - rel), rel);
    for(int i = 0; i < ntake && !known; i++) {
      known = wanted = !strcmp(take[i], name);
    }
    for(int i = 0; i < nskip && !known; i++) {
      known = !strcmp(skip[i], name);
    }
    if(!known) {
      struct stat st;
      snprintf(path, sizeof(path), THEMES "/%s", name);
      /* not there yet, or there in an older version of the one that comes
       * (the user's own files in that folder are kept) */
      wanted = stat(path, &st) != 0 || installed_version(name) < packed_version(name);
      if(wanted && ntake < 16) {
        snprintf(take[ntake++], 64, "%s", name);
      } else if(!wanted && nskip < 16) {
        snprintf(skip[nskip++], 64, "%s", name);
      }
    }
    if(wanted) {
      snprintf(path, sizeof(path), THEMES "/%s", rel);
      mkparents(path);
      if((f = fopen(path, "wb"))) {
        fwrite(p, 1, size, f);
        fclose(f);
        files++;
      }
    }
    p += size;
  }
  if((f = fopen(BUNDLED, "w"))) {
    fprintf(f, "%s\n", id);
    fclose(f);
  }
  note("themes that come with NeoHEN: %d written (%d files), %d already there", ntake, files, nskip);
  /* on a console that had no theme chosen, the first one that came is it */
  if(ntake) {
    struct stat st;
    int chosen = (!stat(ACTIVE, &st) && st.st_size > 1) || !stat(DEFAULT, &st);
    if(!chosen && (f = fopen(ACTIVE, "w"))) {
      fprintf(f, "%s\n", take[0]);
      fclose(f);
    }
  }
}

/* Asks NeoHEN, on its control port, to restart the home menu: only a new
 * menu reads every file of a theme (icons, sounds, music). 0 done, 2 a game
 * or app is open, -1 no NeoHEN that can do it. */
static int
restart_menu(void) {
  const uint32_t frame[2] = {0x4F4E494Fu, 3};
  struct sockaddr_in addr;
  struct timeval tv = {3, 0};
  unsigned char reply = 0xff;
  int s = socket(AF_INET, SOCK_STREAM, 0);

  if(s < 0) {
    return -1;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9048);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if(connect(s, (struct sockaddr *)&addr, sizeof(addr)) ||
     send(s, frame, sizeof(frame), 0) != (ssize_t)sizeof(frame) || recv(s, &reply, 1, 0) != 1) {
    close(s);
    return -1;
  }
  close(s);
  return reply == 0 ? 0 : reply == 2 ? 2 : -1;
}

int
main(void) {
  char shown[640] = "";  /* signature of what the stores hold */
  char menu[64] = "";    /* the live menu's sandbox */
  time_t menu_since = 0; /* when it was first seen */
  int short_lived = 0;   /* menus in a row that died right after starting */
  int uncovered = 0;     /* the live menu has not got the stores yet */
  int settle = 0;        /* ticks left of covering a new menu at every tick */
  int said = 0;
  int restart = 0;       /* 1: the menu is to be restarted, 2: and a game was open */
  unsigned restart_at = 0; /* not before this tick, so the notice is read first */
  int restarted = 0;     /* the next new menu is the one asked for, not a crash */
  char applied[128] = ""; /* the name of the theme last put on */

  kill_previous();
  syscall(SYS_thr_set_name, -1, SELF);
  mkdir(THEMES, 0777);
  mkdir(CURRENT, 0777);
  unlink(LOGFILE);
  unpack_themes();
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
      if(restarted) {
        restarted = 0;
        short_lived = 0;
      } else if(menu[0] && shown[0] && strcmp(shown, "-")) {
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
      settle = 2000 / TICK_MS;
      note("menu %s seen, its process is %ld ms old", menu, menu_age_ms());
    }
    /* its folders show up one after another while it starts: keep at it
     * every tick until the assets are covered, and two seconds more for
     * the rest */
    if(uncovered || settle > 0) {
      cover_menus();
      if(uncovered && assets_covered(menu)) {
        uncovered = 0;
        note("menu %s covered, its process is %ld ms old", menu, menu_age_ms());
      }
      if(settle > 0) {
        settle--;
      }
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
          if(theme[0] && !said) {
            /* the first one, as NeoHEN starts: the home screen is told to
             * load it and blinks */
            say("NeoHEN: tema \"%s\" aplicado. Si el menú parpadea, es normal.\n"
                "Tu consola, tus reglas. A jugar.", name);
          } else if(strcmp(name, applied)) {
            /* chosen now, from NeoWEB or the Toolbox: only a new menu reads
             * all of it, so the menu is restarted (a file added to the theme
             * in use does not do it) */
            say("NeoHEN: %s%s%s. El menu se reinicia en unos segundos para cargarlo entero.",
                theme[0] ? "tema \"" : "aspecto de fabrica", theme[0] ? name : "",
                theme[0] ? "\" aplicado" : "");
            restart = 1;
            restart_at = tick + 4000 / TICK_MS;
          }
          said = 1;
          snprintf(applied, sizeof(applied), "%s", name);
          snprintf(shown, sizeof(shown), "%s", sig);
          write_state(name, "ok");
        }
      }
      cover_menus();

      /* a pass after the notice, so it is read before the menu goes */
      if(restart && tick >= restart_at) {
        int r = restart_menu();
        if(r == 0) {
          note("menu restarted for theme \"%s\"", applied);
          restart = 0;
          restarted = 1;
        } else if(r == 2) {
          if(restart == 1) {
            say("NeoHEN: cierra el juego o la app; el menu se reiniciara entonces para cargar el tema.");
          }
          restart = 2;
        } else {
          note("menu restart: NeoHEN does not answer on its control port");
          restart = 0;
        }
      }
    }
    usleep(TICK_MS * 1000);
  }
  return 0;
}
