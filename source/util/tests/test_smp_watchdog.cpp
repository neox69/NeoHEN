/* NeoHEN: ShadowMount+ hook state from its debug.log. */
#include "test_harness.h"
#include "smp_watchdog.hpp"

#include <string>

using onion::daemon::smp::HookState;
using onion::daemon::smp::hook_state_from_log;

namespace {

/* Lines copied from the user's ShadowMount+ 1.7beta2 log on 13.42. */
constexpr const char *kInstalled =
    "[11:07:00]   [SHELLCORE] hooks installed: fw=13.42 pid=58 "
    "cave=0x5391db70+0x2e8 launch=1 sandbox=1 trampoline=0x5391ddb0 install=1\n";
constexpr const char *kLost =
    "[12:00:00]   [SHELLCORE] AppInstallAll hook is no longer installed\n";

int test_states() {
  TEST_ASSERT_TRUE(hook_state_from_log("") == HookState::Unknown);
  TEST_ASSERT_TRUE(hook_state_from_log("[SLEEP] left by WORKING\n") ==
                   HookState::Unknown);
  TEST_ASSERT_TRUE(hook_state_from_log(kInstalled) == HookState::Installed);
  TEST_ASSERT_TRUE(hook_state_from_log(kLost) == HookState::Lost);
  TEST_ASSERT_TRUE(hook_state_from_log(std::string(kInstalled) + kLost) ==
                   HookState::Lost);
  /* A relaunch installs the hooks again: the latest event wins. */
  TEST_ASSERT_TRUE(hook_state_from_log(std::string(kInstalled) + kLost +
                                       kInstalled) == HookState::Installed);
  return 0;
}

/* From 1.7beta6 ShadowMount+ repairs a lost hook itself (format strings from
 * 1.7beta8 src/sm_shellcore_hooks.c). */
constexpr const char *kRestored =
    "[12:00:05]   [SHELLCORE] lost hooks restored: pid=58\n";
constexpr const char *kRepairFailed[] = {
    "[12:00:05]   [SHELLCORE] hook repair validation failed; bridge retained\n",
    "[12:00:05]   [SHELLCORE] unknown hook bytes: AppInstallAll; repair skipped\n",
    "[12:00:05]   [SHELLCORE] hook repair write failed: AppInstallAll\n",
    "[12:00:05]   [SHELLCORE] hook repair incomplete; shutdown cleanup required\n",
};

int test_self_repair() {
  TEST_ASSERT_TRUE(hook_state_from_log(std::string(kInstalled) + kRestored) ==
                   HookState::Installed);
  for (const char *failed : kRepairFailed) {
    TEST_ASSERT_TRUE(hook_state_from_log(std::string(kInstalled) + failed) ==
                     HookState::Lost);
    /* A later successful repair wins. */
    TEST_ASSERT_TRUE(hook_state_from_log(std::string(kInstalled) + failed +
                                         kRestored) == HookState::Installed);
  }
  /* Lines that only look alike are not hook events. */
  TEST_ASSERT_TRUE(hook_state_from_log(
                       "[SHELLCORE] hook pages retained: cave=1 sites=4\n") ==
                   HookState::Unknown);
  return 0;
}

} // namespace

int test_smp_watchdog_suite() {
  int failed = onion_test_run("smp_watchdog.hook_state", test_states);
  failed += onion_test_run("smp_watchdog.self_repair", test_self_repair);
  return failed;
}
