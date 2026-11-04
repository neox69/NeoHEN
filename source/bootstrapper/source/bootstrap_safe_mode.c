/* NeoHEN: safe mode at start (see onion/safe_mode.h and docs/robustness.md). */

#include "bootstrap_safe_mode.h"

#include "bootstrap_notify.h"

#include <onion/log.h>
#include <onion/safe_mode.h>

int bootstrap_safe_mode_begin(void) {
  unlink(ONION_BOOT_AUTOSTART_DONE_PATH);
  const int failed = onion_boot_attempts_read();
  const bool marker = onion_file_present(ONION_SAFE_MODE_PATH);
  int level = 0;
  if (failed >= ONION_SAFE_MODE_DEEP_BOOTS)
    level = 2;
  else if (failed >= ONION_SAFE_MODE_BOOTS || marker)
    level = 1;

  if (level && !marker) {
    char text[64];
    snprintf(text, sizeof(text), "failed starts in a row: %d\n", failed);
    onion_boot_write_text(ONION_SAFE_MODE_PATH, text);
  }
  /* counted now; the daemon puts it back to 0 at boot OK */
  onion_boot_attempts_write(failed + 1);
  if (level) {
    char now[8];
    snprintf(now, sizeof(now), "%d\n", level);
    onion_boot_write_text(ONION_BOOT_SAFE_NOW_PATH, now);
    LOG_WARN("safe mode (%s): %d failed starts in a row%s; no auto-start "
             "payloads%s, no plugins",
             level == 2 ? "deep" : "normal", failed,
             marker ? ", safe_mode file present" : "",
             level == 2 ? " (NeoWEB too)" : " (NeoWEB still starts)");
    bootstrap_notify(level == 2 ? "notify.safe_mode.deep" : "notify.safe_mode.on");
  } else {
    unlink(ONION_BOOT_SAFE_NOW_PATH);
    if (failed)
      LOG_INFO("previous start did not reach boot OK (%d in a row)", failed);
  }
  return level;
}

void bootstrap_safe_mode_autostart_done(void) {
  onion_boot_write_text(ONION_BOOT_AUTOSTART_DONE_PATH, "1\n");
}
