/* NeoHEN: ShadowMount+ debug.log parsing (host-testable, no PS5 calls). */

#include "smp_watchdog.hpp"

namespace onion::daemon::smp {

namespace {
/* Exact strings from ShadowMount+ 1.7 src/sm_shellcore_hooks.c. */
constexpr std::string_view kInstalled = "[SHELLCORE] hooks installed:";
constexpr std::string_view kLost = "AppInstallAll hook is no longer installed";
} // namespace

HookState hook_state_from_log(std::string_view log_tail) {
  const size_t installed = log_tail.rfind(kInstalled);
  const size_t lost = log_tail.rfind(kLost);
  if (installed == std::string_view::npos && lost == std::string_view::npos)
    return HookState::Unknown;
  if (lost == std::string_view::npos)
    return HookState::Installed;
  if (installed == std::string_view::npos)
    return HookState::Lost;
  return lost > installed ? HookState::Lost : HookState::Installed;
}

} // namespace onion::daemon::smp
