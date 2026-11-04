/* Copyright (C) 2025 OnionHEN / LightningMods

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.
*/

#include <onion/brand.h>
#include "bootstrap_assets.h"
#include "bootstrap_config.h"
#include "bootstrap_filesystem.h"
#include "bootstrap_notify.h"
#include "bootstrap_runtime.h"
#include "bootstrap_safe_mode.h"
#include "bootstrap_update.h"
#include "launch_pipeline.h"
#include "payload_autostart.h"
#include "bundled_payloads.h"
#include "replace_running.h"

#include <onion/conflict.h>
#include <onion/log.h>

int main(void) {
  /*
   * Best-effort file sink: /data/OnionHEN is only created a few lines below,
   * so this first call may not open. It is re-applied once the directory
   * exists; until then records still reach klog, stdout and the remote log.
   */
  onion_log_configure("Bootstrapper", ONION_DATA_DIR "/" ONION_BRAND_NAME ".log");
  onion_log_configure_crash(ONION_DATA_DIR "/" ONION_BRAND_NAME "_crash.log");

  BootstrapConfig config{};
  if (!bootstrap_config_load(&config))
    return -1;

  if (!bootstrap_runtime_prepare())
    return -1;
  bootstrap_runtime_enable_remote_logging();

  const char *conflict = onion_conflict_detect();
  if (conflict) {
    LOG_ERROR("refusing start: %s already running", conflict);
    bootstrap_notify("notify.boot.conflict", conflict);
    return 0;
  }

  /* NeoHEN: an older (or the same) NeoHEN already loaded is shut down first;
   * two daemons and two Toolboxes in one home screen hang the console. */
  if (!bootstrap_replace_running_neohen())
    return 0;

  LOG_DEBUG("============== Spawner (Bootstrapper) Started =================");
  bootstrap_filesystem_create_directories();
  /* The directory exists now, so this pass is the one that sticks. */
  onion_log_configure("Bootstrapper", ONION_DATA_DIR "/" ONION_BRAND_NAME ".log");
  onion_log_configure_crash(ONION_DATA_DIR "/" ONION_BRAND_NAME "_crash.log");
  /* NeoHEN: a newer NeoHEN installed from NeoWEB takes over from here when
   * update.apply_at_boot is on (this start ends; that one counts itself),
   * and a failed one is rolled back. */
  if (!bootstrap_update_begin(config.update_apply_at_boot))
    return 0;

  if (!bootstrap_filesystem_mount_system())
    return -1;

  /* Counted before anything is launched: a start that crashes from here on
   * without reaching the daemon's boot OK counts towards safe mode. */
  const int safe_level = bootstrap_safe_mode_begin();

  LOG_DEBUG("Writing embedded assets ...");
  bootstrap_notify_starting(bootstrap_assets_write(config.system_ex_icon));
  LOG_DEBUG("   Written!");

  bootstrap_filesystem_disable_updates();

  const int launch_result = bootstrap_launch_services(
      config.firmware_version, config.kstuff_autoload);
  if (launch_result != 0)
    return launch_result;

  bootstrap_bundled_payloads_install();
  if (safe_level == 0)
    bootstrap_payload_autostart();
  else if (safe_level == 1)
    bootstrap_payload_autostart_only("neoweb.elf"); /* to turn safe mode off */
  else
    LOG_WARN("Deep safe mode: no payload auto-start at all");
  bootstrap_safe_mode_autostart_done();
  LOG_DEBUG("============== Spawner (Bootstrapper) Finished =================");
  return 0;
}
