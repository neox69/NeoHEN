/* NeoHEN: ShadowMount+ watchdog runtime (see smp_watchdog.hpp). */

#include "smp_watchdog.hpp"

#include "daemon_ops.hpp"
#include "daemon_power_state.hpp"
#include "globalconf.hpp"
#include <onion/brand.h>
#include <onion/payload.h>
#include <onion/platform.h>
#include <onion/proc_query.h>
#include <onion/settings.hpp>

#include <atomic>
#include <cstdio>
#include <pthread.h>
#include <string>
#include <time.h>
#include <unistd.h>

namespace onion::daemon::smp {

namespace {

constexpr const char *kLogPath = "/data/shadowmount/debug.log";
constexpr const char *kElfPath = ONION_DATA_DIR "/payloads/shadowmountplus.elf";
constexpr const char *kAutoStart =
    ONION_DATA_DIR "/payloads/shadowmountplus.elf.auto_start";
constexpr long kTailBytes = 128 * 1024;
constexpr int kFirstCheckSeconds = 120;  // let autostart finish first
constexpr int kResumeCheckSeconds = 60;  // ShadowMount+ resumes on WORKING
constexpr int kRelaunchMinGapSeconds = 600;
constexpr int kRelaunchMaxPerBoot = 3;

std::atomic<bool> g_started{false};
std::atomic<int> g_resume_due{-1}; // seconds until a resume check, -1 = none

enum class Problem { None, HookLost, NotRunning };

std::string read_tail() {
  std::string out;
  FILE *f = fopen(kLogPath, "rb");
  if (!f)
    return out;
  if (fseek(f, 0, SEEK_END) == 0) {
    const long size = ftell(f);
    const long start = size > kTailBytes ? size - kTailBytes : 0;
    if (start >= 0 && fseek(f, start, SEEK_SET) == 0) {
      out.resize(static_cast<size_t>(size - start));
      out.resize(fread(out.data(), 1, out.size(), f));
    }
  }
  fclose(f);
  return out;
}

bool smp_running() {
  /* elfldr names the process after the file; ki_comm keeps 19 chars. */
  return onion_find_pid_substr("shadowmount") > 0 ||
         onion_find_pid_substr("ShadowMount") > 0;
}

Problem check() {
  if (!smp_running())
    return if_exists(kAutoStart) ? Problem::NotRunning : Problem::None;
  return hook_state_from_log(read_tail()) == HookState::Lost ? Problem::HookLost
                                                             : Problem::None;
}

bool relaunch() {
  if (!if_exists(kElfPath)) {
    onion_notify(true, "notify.payload.missing_fmt", "shadowmountplus.elf",
                 ONION_DATA_DIR "/payloads");
    return false;
  }
  char key[ONION_PAYLOAD_IDENTITY_SIZE];
  if (!onion_payload_identity_from_path(kElfPath, key, sizeof(key)))
    return false;
  /* A new instance asks the old one to exit (ShadowMount+ main.c), so this
   * replaces it even if the old process is still alive. */
  const pid_t pid = onion_payload_launch_elfldr(key, kElfPath);
  if (pid > 1) {
    char pid_path[256];
    onion_payload_pid_path(pid_path, sizeof(pid_path), key);
    onion_payload_write_pid_file(pid_path, pid);
  }
  return pid > 1;
}

void *watchdog_thread(void *) {
  Problem last = Problem::None;
  bool reported_ok = false;
  int relaunches = 0;
  time_t last_relaunch = 0;
  int countdown = kFirstCheckSeconds;
  while (!g_stack_shutting_down.load(std::memory_order_acquire)) {
    sleep(1);
    const auto settings = g_settings.snapshot();
    if (settings.smp_watchdog == onion::kSmpWatchdogOff ||
        daemon_power_state_is_sleeping())
      continue;
    const int resume_due = g_resume_due.load(std::memory_order_acquire);
    if (resume_due > 0) {
      g_resume_due.store(resume_due - 1, std::memory_order_release);
      continue;
    }
    const bool resume_check = resume_due == 0;
    if (resume_check)
      g_resume_due.store(-1, std::memory_order_release);
    else if (--countdown > 0)
      continue;
    countdown = settings.smp_watchdog_interval_seconds;

    const Problem problem = check();
    if (problem == Problem::None) {
      if (last != Problem::None) {
        LOG_INFO("smp watchdog: ShadowMount+ healthy again");
      } else if (resume_check || !reported_ok) {
        /* Once per boot and after each Rest Mode, so a log shows it ran. */
        LOG_INFO("smp watchdog: ShadowMount+ %s%s",
                 smp_running() ? "ok" : "not installed",
                 resume_check ? " (after Rest Mode)" : "");
      }
      reported_ok = true;
      last = problem;
      continue;
    }
    const char *what = problem == Problem::HookLost ? "hook lost" : "not running";
    LOG_WARN("smp watchdog: ShadowMount+ %s%s", what,
             resume_check ? " (after Rest Mode)" : "");
    if (settings.smp_watchdog == onion::kSmpWatchdogRelaunch) {
      const time_t now = time(nullptr);
      if (relaunches >= kRelaunchMaxPerBoot ||
          (last_relaunch && now - last_relaunch < kRelaunchMinGapSeconds)) {
        if (problem != last)
          onion_notify(true, "notify.smp.relaunch_limited");
      } else {
        ++relaunches;
        last_relaunch = now;
        const bool ok = relaunch();
        LOG_INFO("smp watchdog: relaunch %s (%d/%d)", ok ? "started" : "failed",
                 relaunches, kRelaunchMaxPerBoot);
        onion_notify(true, ok ? "notify.smp.relaunched"
                              : "notify.smp.relaunch_failed");
      }
    } else if (problem != last) {
      onion_notify(true, problem == Problem::HookLost ? "notify.smp.hook_lost"
                                                      : "notify.smp.not_running");
    }
    last = problem;
  }
  return nullptr;
}

} // namespace

void watchdog_start() {
  if (g_started.exchange(true))
    return;
  pthread_t thread = nullptr;
  if (pthread_create(&thread, nullptr, watchdog_thread, nullptr) == 0)
    pthread_detach(thread);
  else
    LOG_ERROR("smp watchdog: cannot start thread");
}

void watchdog_on_resume() {
  g_resume_due.store(kResumeCheckSeconds, std::memory_order_release);
}

} // namespace onion::daemon::smp
