/* NeoHEN: an update installed from NeoWEB takes over at the next start.
 *
 * NeoHEN.elf is sent to 9021 on every boot, so an update cannot replace
 * it where it is. util keeps the new release in /data/neohen/update/current
 * and this start, before it launches or counts anything, looks there (the
 * rules are onion_update_decide() in onion/update_state.h):
 *
 * - installed release newer than this one: its NeoHEN.elf is checked
 *   against the SHA-256 util wrote and sent to 9021, and this start ends.
 *   The new one then does everything (replacing a running NeoHEN too);
 * - same release: this is it, and while it is on trial the start is counted
 *   in state.json (trial_starts);
 * - the trial release did not reach boot OK twice in a row: previous/ is
 *   put back (or, with none, current/ is taken away), boot_attempts goes
 *   back to 0 so the restored one does not start in safe mode because of the
 *   other's failures (a safe_mode file the user made stays), a notification
 *   says so, and the decision is taken
 *   again with what is left;
 * - older installed release, nothing installed, or a developer build: as
 *   always.
 *
 * Any failure here (no file, bad hash, 9021 not answering) falls back to
 * starting this NeoHEN.elf, the one the user sent. */
#include "bootstrap_update.h"

#include "bootstrap_notify.h"

#include <elfldr_remote.h>
#include <onion/fs.h>
#include <onion/log.h>
#include <onion/safe_mode.h>
#include <onion/update_state.h>
#include <onion/version.h>
#include <sha256.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* NeoHEN.elf is ~16 MB; anything far bigger is not one */
#define HANDOFF_MAX_BYTES (64u << 20)

static void hex32(const uint8_t *d, char out[65]) {
  static const char k[] = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    out[i * 2] = k[d[i] >> 4];
    out[i * 2 + 1] = k[d[i] & 15];
  }
  out[64] = '\0';
}

/* current/NeoHEN.elf to 9021, if it is the file util checked */
static bool hand_off(const char *version) {
  char want[80];
  onion_update_read_word(ONION_UPDATE_CURRENT "/" ONION_UPDATE_SHA_FILE, want, sizeof(want));
  uint8_t *elf = NULL;
  size_t size = 0;
  if (!read_file_alloc(ONION_UPDATE_CURRENT "/" ONION_UPDATE_ELF, HANDOFF_MAX_BYTES, &elf,
                       &size)) {
    LOG_WARN("update: %s is installed but its NeoHEN.elf cannot be read; starting this one",
             version);
    return false;
  }
  SHA256_CTX ctx;
  uint8_t digest[SHA256_BLOCK_SIZE];
  char got[65];
  sha256_init(&ctx);
  sha256_update(&ctx, elf, size);
  sha256_final(&ctx, digest);
  hex32(digest, got);
  if (strlen(want) != 64 || strcmp(got, want) != 0 || size < 4 ||
      memcmp(elf, "\x7f" "ELF", 4) != 0) {
    LOG_WARN("update: installed NeoHEN.elf %s does not match its SHA-256; starting this one",
             version);
    free(elf);
    return false;
  }
  const bool sent = elfldr_remote_send_bytes_to(ELFLDR_REMOTE_PORT, elf, size);
  free(elf);
  if (!sent) {
    LOG_WARN("update: 9021 did not take the installed NeoHEN %s; starting this one", version);
    return false;
  }
  LOG_INFO("update: handed off to the installed NeoHEN %s (this is %s)", version,
           ONIONHEN_VERSION);
  return true;
}

bool bootstrap_update_begin(void) {
  for (int round = 0; round < 2; ++round) {
    onion_update_state st;
    char installed[ONION_UPDATE_VER_MAX];
    onion_update_state_load(ONION_UPDATE_DIR, &st);
    onion_update_read_word(ONION_UPDATE_CURRENT "/" ONION_UPDATE_VERSION_FILE, installed,
                           sizeof(installed));
    const int failed = onion_boot_attempts_read();
    const onion_update_decision d =
        onion_update_decide(&st, installed, ONIONHEN_VERSION, failed);

    if (d.confirm_ok) {
      /* util normally does this at boot OK; it did not get to */
      snprintf(st.status, sizeof(st.status), "ok");
      snprintf(st.current, sizeof(st.current), "%s", installed);
      st.confirmed_at = (long)time(NULL);
      onion_update_state_save(ONION_UPDATE_DIR, &st);
      LOG_INFO("update: %s reached boot OK last time: kept", installed);
    }

    if (d.action == ONION_UPDATE_ROLL_BACK) {
      LOG_WARN("update: %s did not finish starting %d times in a row: rolling back",
               installed, failed);
      if (!onion_update_roll_back(ONION_UPDATE_DIR, &st, "auto", (long)time(NULL))) {
        LOG_ERROR("update: the rollback could not move the files; starting this one");
        return true;
      }
      /* its failures are not the restored one's */
      onion_boot_attempts_write(0);
      const bool restored_runs =
          st.current[0] && onion_update_version_cmp(st.current, ONIONHEN_VERSION) > 0;
      char now_runs[ONION_UPDATE_VER_MAX];
      snprintf(now_runs, sizeof(now_runs), "%s", restored_runs ? st.current : ONIONHEN_VERSION);
      now_runs[strcspn(now_runs, "+")] = '\0';
      bootstrap_notify("notify.update.rolled_back_fmt", installed, now_runs);
      continue; /* decide again with what is installed now */
    }

    if (d.action == ONION_UPDATE_HAND_OFF)
      return !hand_off(installed);

    if (d.count_trial) {
      if (onion_update_version_cmp(st.current, installed) != 0) {
        snprintf(st.current, sizeof(st.current), "%s", installed);
        st.trial_starts = 0;
      }
      snprintf(st.status, sizeof(st.status), "trial");
      st.trial_starts++;
      onion_update_state_save(ONION_UPDATE_DIR, &st);
      LOG_INFO("update: %s on trial, start %d (rolled back after %d failed in a row)",
               installed, st.trial_starts, ONION_UPDATE_TRIAL_FAILS);
    }
    return true;
  }
  return true;
}
