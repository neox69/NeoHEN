/* NeoHEN: ShadowMount+ watchdog.
 *
 * ShadowMount+ (drakmor, GPLv3) hooks SceShellCore and logs its hook state in
 * /data/shadowmount/debug.log. Up to 1.7beta5 it never reinstalled a lost
 * hook by itself; from 1.7beta6 it tries, and logs when that fails. A new
 * instance replaces the old one either way. The watchdog only reads that log and
 * the process list; with shadowmount.watchdog=relaunch it starts
 * payloads/shadowmountplus.elf again.
 */
#pragma once

#include <string_view>

namespace onion::daemon::smp {

enum class HookState { Unknown, Installed, Lost };

/** Latest hook event in a debug.log tail. */
HookState hook_state_from_log(std::string_view log_tail);

void watchdog_start();
/** Schedule a check shortly after Rest Mode resume. */
void watchdog_on_resume();

} // namespace onion::daemon::smp
