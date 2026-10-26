/* NeoHEN: ShadowMount+ debug.log parsing (host-testable, no PS5 calls). */

#include "smp_watchdog.hpp"

namespace onion::daemon::smp {

namespace {
/* Exact strings from ShadowMount+ src/sm_shellcore_hooks.c. Up to 1.7beta4 a
 * lost hook was only reported; from 1.7beta6 ShadowMount+ repairs it itself
 * and logs the result, so only a failed repair counts as lost. */
constexpr std::string_view kInstalled[] = {
    "[SHELLCORE] hooks installed:",
    "[SHELLCORE] lost hooks restored:", // 1.7beta6+
};
constexpr std::string_view kLost[] = {
    "AppInstallAll hook is no longer installed", // up to 1.7beta5
    "[SHELLCORE] hook repair validation failed", // 1.7beta6+
    "[SHELLCORE] unknown hook bytes:",
    "[SHELLCORE] hook repair write failed:",
    "[SHELLCORE] hook repair incomplete",
};

/* Position of the latest of `needles` in `hay`, or npos. */
template <size_t N>
size_t latest(std::string_view hay, const std::string_view (&needles)[N]) {
  size_t best = std::string_view::npos;
  for (std::string_view n : needles) {
    const size_t at = hay.rfind(n);
    if (at != std::string_view::npos && (best == std::string_view::npos || at > best))
      best = at;
  }
  return best;
}
} // namespace

HookState hook_state_from_log(std::string_view log_tail) {
  const size_t installed = latest(log_tail, kInstalled);
  const size_t lost = latest(log_tail, kLost);
  if (installed == std::string_view::npos && lost == std::string_view::npos)
    return HookState::Unknown;
  if (lost == std::string_view::npos)
    return HookState::Installed;
  if (installed == std::string_view::npos)
    return HookState::Lost;
  return lost > installed ? HookState::Lost : HookState::Installed;
}

} // namespace onion::daemon::smp
