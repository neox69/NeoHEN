/* NeoHEN: NeoWEB watchdog.
 *
 * NeoWEB (payloads/neoweb.elf, port 8686) is a payload like any other: when
 * it dies nobody starts it again, and on 0.1.0-rc13 it was found gone after
 * some Rest Modes, to be started by hand from the Toolbox. While its
 * autostart flag is set, the watchdog asks it for /api/ping now and then and
 * after each Rest Mode, and starts it again when it is gone (or kills and
 * starts it when it is there but has stopped answering for a few minutes).
 */
#pragma once

namespace onion::daemon::neoweb {

void watchdog_start();
/** Schedule a check shortly after Rest Mode resume. */
void watchdog_on_resume();

} // namespace onion::daemon::neoweb
