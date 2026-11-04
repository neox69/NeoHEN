/* NeoHEN: the NeoHEN installed from NeoWEB, and its rollback.
 *
 * NeoHEN.elf is not installed anywhere: the user sends it to the exploit's
 * ELF loader (9021) on every boot. An update from NeoWEB (util's
 * neohen_update.cpp) therefore keeps the new release on the console:
 *
 *   /data/neohen/update/current/   NeoHEN.elf, version.txt, sha256.txt, ...
 *   /data/neohen/update/previous/  the one installed before it (one only)
 *   /data/neohen/update/state.json this file's struct
 *
 * and every start of NeoHEN.elf (bootstrap_update.c), before it counts
 * itself for safe mode, looks there: when the installed release is newer
 * than the NeoHEN.elf that was sent, it sends the installed one to 9021 and
 * ends ("hand-off"). The ELF the user sends is therefore always the last
 * fallback: a broken install never stops them from starting what they send.
 *
 * A new install is "trial" until one of its starts reaches boot OK (see
 * onion/safe_mode.h; util marks it "ok"). Two starts of it in a row that do
 * not (boot_attempts >= 2 with trial_starts >= 2) and the next start puts
 * previous/ back in place of current/ (or, with none, just takes current/
 * away) and says so. See docs/robustness.md.
 *
 * Header-only C: the bootstrapper (C) and util (C++) both use it, so the
 * state file has one reader and one writer implementation. */
#pragma once

#include <onion/brand.h>
#include <onion/tree.h>

#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ONION_UPDATE_SUBDIR "/update"
#define ONION_UPDATE_DIR ONION_DATA_DIR ONION_UPDATE_SUBDIR
#define ONION_UPDATE_CURRENT ONION_UPDATE_DIR "/current"
#define ONION_UPDATE_PREVIOUS ONION_UPDATE_DIR "/previous"
#define ONION_UPDATE_STATE_PATH ONION_UPDATE_DIR "/state.json"
/* inside current/ and previous/ */
#define ONION_UPDATE_ELF "NeoHEN.elf"
#define ONION_UPDATE_VERSION_FILE "version.txt"
#define ONION_UPDATE_SHA_FILE "sha256.txt"
/* failed starts of a trial release before it is rolled back */
#define ONION_UPDATE_TRIAL_FAILS 2
/* In plain text in NeoHEN.elf (the unpacker, source/unpacker/source/main.c),
 * followed by ONIONHEN_VERSION: everything else is LZMA-compressed, and the
 * updater checks that the file it got is the release it asked for. */
#define ONION_UPDATE_MARKER "NEOHEN-RELEASE-VERSION:"

#define ONION_UPDATE_VER_MAX 48

typedef struct onion_update_state {
  char status[16];                         /* "", trial, ok, rolled_back */
  char current[ONION_UPDATE_VER_MAX];      /* release in current/ */
  char previous[ONION_UPDATE_VER_MAX];     /* release in previous/ */
  char from[ONION_UPDATE_VER_MAX];         /* what was running at install */
  char rolled_back_from[ONION_UPDATE_VER_MAX];
  char reason[16];                         /* auto, manual */
  int trial_starts;                        /* starts of current while trial */
  long installed_at, rolled_back_at, confirmed_at;
} onion_update_state;

/* 0.1.0-rc7 -> {0,1,0,7}; a final release ranks above its rcs, "+sha"
 * build metadata is ignored, any other pre-release ranks below every rc.
 * Same rule as util's update notice and NeoWEB. False when not a version
 * (a "dirty-abcd" developer build). */
static inline bool onion_update_version_key(const char *s, long out[4]) {
  if (!s)
    return false;
  if (*s == 'v')
    ++s;
  for (int i = 0; i < 3; ++i) {
    char *end = NULL;
    if (!isdigit((unsigned char)*s))
      return false;
    out[i] = strtol(s, &end, 10);
    s = end;
    if (i < 2) {
      if (*s != '.')
        return false;
      ++s;
    }
  }
  out[3] = 1000000;
  if (strncmp(s, "-rc", 3) == 0 && isdigit((unsigned char)s[3]))
    out[3] = strtol(s + 3, NULL, 10);
  else if (*s == '-')
    out[3] = 0;
  else if (*s && *s != '+')
    return false;
  return true;
}

/* <0, 0, >0; -2 when either is not a version */
static inline int onion_update_version_cmp(const char *a, const char *b) {
  long ka[4], kb[4];
  if (!onion_update_version_key(a, ka) || !onion_update_version_key(b, kb))
    return -2;
  for (int i = 0; i < 4; ++i)
    if (ka[i] != kb[i])
      return ka[i] < kb[i] ? -1 : 1;
  return 0;
}

/* only what a version or a status word can hold: keeps the file plain */
static inline void onion_update_clean(char *dst, size_t cap, const char *src) {
  size_t n = 0;
  for (; src && *src && n + 1 < cap; ++src)
    if (isalnum((unsigned char)*src) || *src == '.' || *src == '-' || *src == '+' ||
        *src == '_')
      dst[n++] = *src;
  if (cap)
    dst[n] = '\0';
}

/* "key": "value" or "key": 123 from a flat JSON object; false when absent */
static inline bool onion_update_json_raw(const char *text, const char *key, char *out,
                                         size_t cap) {
  char pat[40];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = text ? strstr(text, pat) : NULL;
  if (!p || !cap)
    return false;
  p += strlen(pat);
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    ++p;
  if (*p++ != ':')
    return false;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    ++p;
  size_t n = 0;
  if (*p == '"') {
    for (++p; *p && *p != '"' && n + 1 < cap; ++p)
      out[n++] = *p;
  } else {
    for (; *p && *p != ',' && *p != '}' && !isspace((unsigned char)*p) && n + 1 < cap; ++p)
      out[n++] = *p;
  }
  out[n] = '\0';
  return true;
}

static inline void onion_update_json_str(const char *text, const char *key, char *out,
                                         size_t cap) {
  char raw[96];
  if (!onion_update_json_raw(text, key, raw, sizeof(raw)))
    raw[0] = '\0';
  onion_update_clean(out, cap, raw);
}

static inline long onion_update_json_long(const char *text, const char *key) {
  char raw[32];
  if (!onion_update_json_raw(text, key, raw, sizeof(raw)))
    return 0;
  return strtol(raw, NULL, 10);
}

static inline void onion_update_state_parse(const char *text, onion_update_state *st) {
  memset(st, 0, sizeof(*st));
  if (!text)
    return;
  onion_update_json_str(text, "status", st->status, sizeof(st->status));
  onion_update_json_str(text, "current", st->current, sizeof(st->current));
  onion_update_json_str(text, "previous", st->previous, sizeof(st->previous));
  onion_update_json_str(text, "from", st->from, sizeof(st->from));
  onion_update_json_str(text, "rolled_back_from", st->rolled_back_from,
                        sizeof(st->rolled_back_from));
  onion_update_json_str(text, "reason", st->reason, sizeof(st->reason));
  st->trial_starts = (int)onion_update_json_long(text, "trial_starts");
  if (st->trial_starts < 0)
    st->trial_starts = 0;
  st->installed_at = onion_update_json_long(text, "installed_at");
  st->rolled_back_at = onion_update_json_long(text, "rolled_back_at");
  st->confirmed_at = onion_update_json_long(text, "confirmed_at");
}

static inline int onion_update_state_format(const onion_update_state *st, char *out,
                                            size_t cap) {
  return snprintf(out, cap,
                  "{\"status\":\"%s\",\"current\":\"%s\",\"previous\":\"%s\","
                  "\"from\":\"%s\",\"trial_starts\":%d,\"installed_at\":%ld,"
                  "\"confirmed_at\":%ld,\"rolled_back_from\":\"%s\","
                  "\"rolled_back_at\":%ld,\"reason\":\"%s\"}\n",
                  st->status, st->current, st->previous, st->from, st->trial_starts,
                  st->installed_at, st->confirmed_at, st->rolled_back_from,
                  st->rolled_back_at, st->reason);
}

/* the first line of a small text file, cleaned; "" when there is none */
static inline void onion_update_read_word(const char *path, char *out, size_t cap) {
  char line[128] = {0};
  FILE *f = fopen(path, "r");
  if (f) {
    if (!fgets(line, sizeof(line), f))
      line[0] = '\0';
    fclose(f);
  }
  onion_update_clean(out, cap, line);
}

/* @dir: ONION_UPDATE_DIR, or the same folder as util sees it */
static inline bool onion_update_state_load(const char *dir, onion_update_state *st) {
  char path[256], text[1024] = {0};
  snprintf(path, sizeof(path), "%s/state.json", dir);
  FILE *f = fopen(path, "r");
  if (!f) {
    onion_update_state_parse(NULL, st);
    return false;
  }
  const size_t n = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  text[n] = '\0';
  onion_update_state_parse(text, st);
  return true;
}

static inline bool onion_update_state_save(const char *dir, const onion_update_state *st) {
  char path[256], tmp[264], text[1024];
  snprintf(path, sizeof(path), "%s/state.json", dir);
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  const int len = onion_update_state_format(st, text, sizeof(text));
  if (len <= 0 || (size_t)len >= sizeof(text))
    return false;
  const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    return false;
  const bool wrote = write(fd, text, (size_t)len) == (ssize_t)len;
  const bool synced = fsync(fd) == 0;
  if (close(fd) != 0 || !wrote || !synced || rename(tmp, path) != 0) {
    unlink(tmp);
    return false;
  }
  return true;
}

/* What one start of NeoHEN.elf does with what is installed. */
typedef enum onion_update_action {
  ONION_UPDATE_RUN_SELF = 0, /* nothing newer installed: go on as usual */
  ONION_UPDATE_HAND_OFF,     /* send current/NeoHEN.elf to 9021 and end */
  ONION_UPDATE_ROLL_BACK,    /* the trial failed twice: put previous back */
} onion_update_action;

typedef struct onion_update_decision {
  onion_update_action action;
  bool confirm_ok;  /* the last trial start reached boot OK: mark it ok */
  bool count_trial; /* this start runs the trial release: count it */
} onion_update_decision;

/* @installed: version.txt of current/ ("" when there is none), @mine: this
 * NeoHEN.elf's ONIONHEN_VERSION, @boot_attempts: the count before this start
 * adds itself (failed starts in a row). Pure: no files. */
static inline onion_update_decision onion_update_decide(const onion_update_state *st,
                                                        const char *installed,
                                                        const char *mine,
                                                        int boot_attempts) {
  onion_update_decision d = {ONION_UPDATE_RUN_SELF, false, false};
  if (!installed || !installed[0] || strcmp(st->status, "rolled_back") == 0)
    return d;
  const int cmp = onion_update_version_cmp(installed, mine);
  if (cmp == -2 || cmp < 0)
    return d; /* a developer build, or a newer NeoHEN.elf was sent: it wins */
  /* current/ that state.json does not describe was swapped in right before
   * a crash: it has not been confirmed, so it is on trial from 0 */
  const bool described = onion_update_version_cmp(st->current, installed) == 0;
  const bool trial = !described || strcmp(st->status, "ok") != 0;
  if (trial) {
    const int starts = described ? st->trial_starts : 0;
    if (starts >= 1 && boot_attempts == 0) {
      d.confirm_ok = true;
    } else if (starts >= ONION_UPDATE_TRIAL_FAILS &&
               boot_attempts >= ONION_UPDATE_TRIAL_FAILS) {
      d.action = ONION_UPDATE_ROLL_BACK;
      return d;
    }
  }
  if (cmp > 0)
    d.action = ONION_UPDATE_HAND_OFF;
  else
    d.count_trial = trial && !d.confirm_ok;
  return d;
}

/* Puts previous/ back as current/ (or, with no previous/, takes current/
 * away, so the NeoHEN.elf that is sent runs) and records it. @dir is
 * ONION_UPDATE_DIR as the caller sees it. The release taken out is deleted:
 * only one older version is ever kept. False when nothing could be moved. */
static inline bool onion_update_roll_back(const char *dir, onion_update_state *st,
                                          const char *reason, long now) {
  char cur[256], prev[256], failed[256], ver[ONION_UPDATE_VER_MAX];
  struct stat sb;
  snprintf(cur, sizeof(cur), "%s/current", dir);
  snprintf(prev, sizeof(prev), "%s/previous", dir);
  snprintf(failed, sizeof(failed), "%s/failed", dir);
  char vpath[300];
  snprintf(vpath, sizeof(vpath), "%s/" ONION_UPDATE_VERSION_FILE, cur);
  onion_update_read_word(vpath, ver, sizeof(ver));
  if (stat(failed, &sb) == 0)
    rmtree(failed);
  const bool had_cur = stat(cur, &sb) == 0;
  if (had_cur && rename(cur, failed) != 0)
    return false;
  onion_update_state out;
  memset(&out, 0, sizeof(out));
  snprintf(out.rolled_back_from, sizeof(out.rolled_back_from), "%s",
           ver[0] ? ver : st->current);
  snprintf(out.from, sizeof(out.from), "%s", st->from);
  onion_update_clean(out.reason, sizeof(out.reason), reason);
  out.rolled_back_at = now;
  if (stat(prev, &sb) == 0) {
    if (rename(prev, cur) != 0) {
      if (had_cur)
        rename(failed, cur);
      return false;
    }
    snprintf(vpath, sizeof(vpath), "%s/" ONION_UPDATE_VERSION_FILE, cur);
    onion_update_read_word(vpath, out.current, sizeof(out.current));
    snprintf(out.status, sizeof(out.status), "%s", out.current[0] ? "ok" : "rolled_back");
    out.confirmed_at = now;
  } else {
    snprintf(out.status, sizeof(out.status), "rolled_back");
  }
  *st = out;
  const bool saved = onion_update_state_save(dir, st);
  if (stat(failed, &sb) == 0)
    rmtree(failed);
  return saved;
}
