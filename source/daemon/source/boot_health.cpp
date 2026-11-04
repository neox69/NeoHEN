/* NeoHEN: boot OK and conflict warning (see boot_health.hpp). */

#include "boot_health.hpp"

#include "daemon_ops.hpp"
#include <onion/log.h>
#include <onion/notify.h>
#include <onion/proc_query.h>
#include <onion/safe_mode.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <pthread.h>
#include <string>
#include <unistd.h>

namespace onion::daemon::boot_health {

namespace {

constexpr int kPollSeconds = 5;
constexpr int kAutostartWaitMaxSeconds = 30 * 60; // the bootstrapper died
constexpr int kSecondConflictCheckSeconds = 5 * 60;

std::atomic<bool> g_started{false};

struct Scan {
  int kstuff = 0;
  bool backpork = false, etahen = false, cheatrunner = false;
};

bool has(const char *name, const char *low_needle) {
  if (!name || !name[0])
    return false;
  char low[64];
  size_t i = 0;
  for (; name[i] && i < sizeof(low) - 1; ++i)
    low[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
  low[i] = '\0';
  return std::strstr(low, low_needle) != nullptr;
}

int scan_cb(void *ctx, pid_t pid, const char *comm, const char *tdname) {
  auto *s = static_cast<Scan *>(ctx);
  if (pid == getpid())
    return 0;
  auto any = [&](const char *needle) { return has(comm, needle) || has(tdname, needle); };
  if (any("kstuff"))
    ++s->kstuff;
  s->backpork = s->backpork || any("backpork");
  s->etahen = s->etahen || any("etahen");
  s->cheatrunner = s->cheatrunner || any("cheatrunner");
  return 0;
}

/* "BackPork, a second kstuff", or "" */
std::string conflicts() {
  Scan s;
  if (onion_proc_each(scan_cb, &s) < 0)
    return "";
  std::string out;
  auto add = [&](const char *what) {
    if (!out.empty())
      out += ", ";
    out += what;
  };
  if (s.backpork)
    add("BackPork");
  if (s.etahen)
    add("etaHEN");
  if (s.cheatrunner)
    add("CheatRunner");
  if (s.kstuff > 1)
    add("a second kstuff");
  return out;
}

void warn_conflicts(std::string &said) {
  const std::string now = conflicts();
  if (now.empty() || now == said)
    return;
  said = now;
  LOG_WARN("conflict: also running: %s (it can clash with NeoHEN; nothing was stopped)",
           now.c_str());
  onion_notify(true, "notify.conflict.running_fmt", now.c_str());
}

bool shutting_down() {
  return g_stack_shutting_down.load(std::memory_order_acquire);
}

void *boot_health_thread(void *) {
  const int level = onion_boot_safe_level();
  int waited = 0;
  while (!onion_file_present(ONION_BOOT_AUTOSTART_DONE_PATH)) {
    if (shutting_down() || waited >= kAutostartWaitMaxSeconds) {
      LOG_WARN("boot: auto-start did not finish; this start is not boot OK");
      return nullptr;
    }
    sleep(kPollSeconds);
    waited += kPollSeconds;
  }
  for (int i = 0; i < ONION_BOOT_OK_SECONDS && !shutting_down(); ++i)
    sleep(1);
  if (shutting_down())
    return nullptr;

  onion_boot_attempts_write(0);
  if (level >= 2) {
    /* no NeoWEB to turn it off from: a start that holds is the way out */
    unlink(ONION_SAFE_MODE_PATH);
    LOG_INFO("boot OK (deep safe mode): safe mode cleared for the next start");
  } else if (level == 1) {
    LOG_INFO("boot OK in safe mode; it stays on until NeoWEB's Normal start");
  } else {
    LOG_INFO("boot OK");
  }

  std::string said;
  warn_conflicts(said);
  for (int i = 0; i < kSecondConflictCheckSeconds && !shutting_down(); ++i)
    sleep(1);
  if (!shutting_down())
    warn_conflicts(said);
  return nullptr;
}

} // namespace

bool safe_mode() { return onion_boot_safe_level() > 0; }

void start() {
  if (g_started.exchange(true))
    return;
  pthread_t thread = nullptr;
  if (pthread_create(&thread, nullptr, boot_health_thread, nullptr) == 0)
    pthread_detach(thread);
  else
    LOG_ERROR("boot: cannot start the boot OK thread");
}

} // namespace onion::daemon::boot_health
