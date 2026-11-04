/* NeoHEN: boot OK (safe mode's counter) and the conflict warning.
 *
 * Boot OK: once the bootstrapper has finished auto-starting payloads and
 * this daemon is still up ONION_BOOT_OK_SECONDS later, the start counts as
 * good: boot_attempts goes back to 0 (see onion/safe_mode.h).
 * Conflicts: at that same point, and again after a few minutes, it looks
 * for homebrew that clashes with NeoHEN (BackPork, etaHEN, CheatRunner, a
 * second kstuff) and says so in a notification. It never stops anything.
 */
#pragma once

namespace onion::daemon::boot_health {

void start();
/** This start is a safe-mode one: plugins are not started. */
bool safe_mode();

} // namespace onion::daemon::boot_health
