/* NeoHEN: safe mode, shared by the bootstrapper (NeoHEN.elf) and the daemon.
 *
 * Every NeoHEN start counts itself in boot_attempts before it launches
 * anything. The daemon puts the count back to 0 once the start is "boot OK":
 * the bootstrapper finished auto-starting payloads (it writes boot_autostart_
 * done) and NeoHEN is still alive ONION_BOOT_OK_SECONDS later. A crash or a
 * hang before that leaves the count up.
 *
 * With ONION_SAFE_MODE_BOOTS failed starts in a row, or with a safe_mode file
 * present, the start is a safe one: no auto-start payloads (NeoWEB, NeoHEN's
 * own page, still starts, so safe mode can be turned off from it) and no
 * plugins (ELF plugins' auto-start, SPRX plugins). If even those starts fail
 * up to ONION_SAFE_MODE_DEEP_BOOTS, NeoWEB is left out too.
 *
 * safe_mode stays until NeoWEB's "Normal start" button (or the user, over
 * FTP) removes it, so a crashing payload does not get three more tries every
 * other boot. Only a deep safe start (no NeoWEB to turn it off from) clears
 * it by itself once it reaches boot OK. A normal start that reaches boot OK
 * just puts the count back to 0. See docs/robustness.md. */
#pragma once

#include <onion/brand.h>

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define ONION_SAFE_MODE_BOOTS 3
#define ONION_SAFE_MODE_DEEP_BOOTS 5
#define ONION_BOOT_OK_SECONDS 60
#define ONION_BOOT_ATTEMPTS_PATH ONION_DATA_DIR "/boot_attempts"
#define ONION_SAFE_MODE_PATH ONION_DATA_DIR "/safe_mode"
#define ONION_BOOT_AUTOSTART_DONE_PATH ONION_DATA_DIR "/boot_autostart_done"
/* written for this start only: what the bootstrapper decided */
#define ONION_BOOT_SAFE_NOW_PATH ONION_DATA_DIR "/boot_safe_now"

static inline int onion_boot_attempts_read(void) {
  int n = 0;
  FILE *f = fopen(ONION_BOOT_ATTEMPTS_PATH, "r");
  if (f) {
    if (fscanf(f, "%d", &n) != 1)
      n = 0;
    fclose(f);
  }
  return n < 0 ? 0 : n;
}

static inline void onion_boot_write_text(const char *path, const char *text) {
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "w");
  if (!f)
    return;
  const bool ok = fputs(text, f) >= 0;
  if (fclose(f) == 0 && ok)
    rename(tmp, path);
  else
    unlink(tmp);
}

static inline void onion_boot_attempts_write(int n) {
  if (n <= 0) {
    unlink(ONION_BOOT_ATTEMPTS_PATH);
    return;
  }
  char text[16];
  snprintf(text, sizeof(text), "%d\n", n);
  onion_boot_write_text(ONION_BOOT_ATTEMPTS_PATH, text);
}

static inline bool onion_file_present(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

/* What this start is, as the bootstrapper decided it: 0 normal, 1 safe
 * (NeoWEB still starts), 2 deep safe (NeoWEB left out too). */
static inline int onion_boot_safe_level(void) {
  int level = 0;
  FILE *f = fopen(ONION_BOOT_SAFE_NOW_PATH, "r");
  if (f) {
    if (fscanf(f, "%d", &level) != 1)
      level = 0;
    fclose(f);
  }
  return level < 0 ? 0 : level;
}
