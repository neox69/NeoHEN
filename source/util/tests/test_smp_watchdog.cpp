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

} // namespace

int test_smp_watchdog_suite() {
  return onion_test_run("smp_watchdog.hook_state", test_states);
}
