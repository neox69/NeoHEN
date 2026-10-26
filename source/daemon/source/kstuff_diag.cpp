/* NeoHEN: read-only diagnostic of kstuff's syscall-table switches.
 *
 * kstuff-lite marks its PS5 and PS4 sysentvec as active by writing 0xdeb7 at
 * sysentvec+14 (0xffff = paused). ShadowMount+ up to 1.7beta4 paused/resumed
 * kstuff by toggling the same field (its "pause deadline reached ... kstuff
 * was already disabled" messages); 1.7beta5 dropped that, so anything that
 * changes it now is kstuff itself or another payload.
 * Only kernel reads; enabled by diagnostics.kstuff_tables (default false).
 */

#include "daemon_ops.hpp"
#include "globalconf.hpp"
#include <onion/platform.h>
#include <onion/settings.hpp>
#include <ps5/kernel.h>

#include <pthread.h>
#include <unistd.h>

namespace {

constexpr intptr_t kToggleOffset = 14;

/* Offsets from kstuff-lite prosper0gdb/offsets (13_20.h, 13_42.h), the same
 * values ShadowMount+ 1.7beta4 used. Other firmware: not known here. */
bool sysentvec_offsets(uint32_t fw, intptr_t *ps5, intptr_t *ps4) {
  switch (fw & 0xffff0000u) {
  case 0x13200000:
    *ps5 = 0xddd8b8;
    *ps4 = 0xddda30;
    return true;
  case 0x13400000:
  case 0x13420000:
  case 0x13600000:
    *ps5 = 0xddd8f8;
    *ps4 = 0xddda70;
    return true;
  default:
    return false;
  }
}

const char *describe(uint16_t v) {
  return v == 0xdeb7 ? "active" : v == 0xffff ? "paused" : "unknown";
}

void *kstuff_diag_thread(void *) {
  intptr_t ps5 = 0, ps4 = 0;
  const uint32_t fw = kernel_get_fw_version();
  if (!sysentvec_offsets(fw, &ps5, &ps4)) {
    LOG_INFO("kstuff diag: no sysentvec offsets for fw 0x%08x", fw);
    return nullptr;
  }
  ps5 += KERNEL_ADDRESS_DATA_BASE;
  ps4 += KERNEL_ADDRESS_DATA_BASE;
  uint32_t last = 0xffffffffu;
  while (!g_stack_shutting_down.load(std::memory_order_acquire)) {
    const uint16_t v5 = kernel_getshort(ps5 + kToggleOffset);
    const uint16_t v4 = kernel_getshort(ps4 + kToggleOffset);
    const uint32_t now = (static_cast<uint32_t>(v5) << 16) | v4;
    if (now != last) {
      LOG_INFO("kstuff diag: ps5 sysentvec=0x%04x (%s) ps4 sysentvec=0x%04x "
               "(%s)",
               v5, describe(v5), v4, describe(v4));
      last = now;
    }
    sleep(2);
  }
  return nullptr;
}

} // namespace

void kstuff_diag_start() {
  if (!g_settings.snapshot().diag_kstuff_tables)
    return;
  pthread_t thread = nullptr;
  if (pthread_create(&thread, nullptr, kstuff_diag_thread, nullptr) == 0)
    pthread_detach(thread);
  else
    LOG_ERROR("kstuff diag: cannot start thread");
}
