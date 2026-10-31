/* NeoHEN: NeoWEB watchdog runtime (see neoweb_watchdog.hpp). */

#include "neoweb_watchdog.hpp"

#include "daemon_ops.hpp"
#include "daemon_power_state.hpp"
#include "globalconf.hpp"
#include <onion/brand.h>
#include <onion/payload.h>
#include <onion/platform.h>
#include <onion/proc_query.h>

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

namespace onion::daemon::neoweb {

namespace {

constexpr const char *kElfPath = ONION_DATA_DIR "/payloads/neoweb.elf";
constexpr const char *kAutoStart = ONION_DATA_DIR "/payloads/neoweb.elf.auto_start";
constexpr int kPort = 8686;
constexpr int kFirstCheckSeconds = 90;  // let autostart finish first
constexpr int kCheckSeconds = 60;
constexpr int kResumeCheckSeconds = 20; // the network is back by then
constexpr int kHungMisses = 3;          // alive but silent this many checks
constexpr int kRelaunchMinGapSeconds = 60;
constexpr int kRelaunchMaxPerBoot = 10;

std::atomic<bool> g_started{false};
std::atomic<int> g_resume_due{-1}; // seconds until a resume check, -1 = none

/* GET /api/ping on this console; true for any HTTP answer. */
bool answers() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return true; // cannot tell; do not act on it
  struct timeval tv = {3, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool ok = false;
  if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
    static const char kReq[] = "GET /api/ping HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    char buf[16] = {};
    if (send(fd, kReq, sizeof(kReq) - 1, 0) == (ssize_t)(sizeof(kReq) - 1) &&
        recv(fd, buf, sizeof(buf) - 1, 0) > 0)
      ok = strncmp(buf, "HTTP/", 5) == 0;
  }
  close(fd);
  return ok;
}

pid_t neoweb_pid() {
  /* elfldr names the process after the file. */
  return onion_find_pid_substr("neoweb");
}

bool relaunch() {
  if (!if_exists(kElfPath)) {
    onion_notify(true, "notify.payload.missing_fmt", "neoweb.elf",
                 ONION_DATA_DIR "/payloads");
    return false;
  }
  char key[ONION_PAYLOAD_IDENTITY_SIZE];
  if (!onion_payload_identity_from_path(kElfPath, key, sizeof(key)))
    return false;
  const pid_t pid = onion_payload_launch_elfldr(key, kElfPath);
  if (pid > 1) {
    char pid_path[256];
    onion_payload_pid_path(pid_path, sizeof(pid_path), key);
    onion_payload_write_pid_file(pid_path, pid);
  }
  return pid > 1;
}

void *watchdog_thread(void *) {
  int countdown = kFirstCheckSeconds;
  int misses = 0;
  int relaunches = 0;
  time_t last_relaunch = 0;
  bool limited_said = false;
  while (!g_stack_shutting_down.load(std::memory_order_acquire)) {
    sleep(1);
    if (daemon_power_state_is_sleeping()) {
      misses = 0;
      continue;
    }
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
    countdown = kCheckSeconds;

    if (!if_exists(kAutoStart) || answers()) {
      if (misses)
        LOG_INFO("neoweb watchdog: NeoWEB answers again");
      else if (resume_check && if_exists(kAutoStart))
        LOG_INFO("neoweb watchdog: NeoWEB ok (after Rest Mode)");
      misses = 0;
      continue;
    }

    const pid_t pid = neoweb_pid();
    ++misses;
    if (pid > 1 && misses < kHungMisses) {
      LOG_WARN("neoweb watchdog: NeoWEB pid=%d does not answer on %d (%d/%d)%s",
               pid, kPort, misses, kHungMisses,
               resume_check ? " (after Rest Mode)" : "");
      continue;
    }
    LOG_WARN("neoweb watchdog: NeoWEB %s%s", pid > 1 ? "hung" : "not running",
             resume_check ? " (after Rest Mode)" : "");

    const time_t now = time(nullptr);
    if (relaunches >= kRelaunchMaxPerBoot ||
        (last_relaunch && now - last_relaunch < kRelaunchMinGapSeconds)) {
      if (!limited_said) {
        LOG_WARN("neoweb watchdog: relaunch limit reached (%d)", relaunches);
        limited_said = true;
      }
      continue;
    }
    if (pid > 1) {
      (void)kill(pid, SIGKILL);
      for (int i = 0; i < 20 && onion_proc_is_alive(pid); ++i)
        usleep(100 * 1000);
    }
    ++relaunches;
    last_relaunch = now;
    misses = 0;
    const bool ok = relaunch();
    LOG_INFO("neoweb watchdog: relaunch %s (%d/%d)", ok ? "started" : "failed",
             relaunches, kRelaunchMaxPerBoot);
    onion_notify(true, ok ? "notify.neoweb.relaunched"
                          : "notify.neoweb.relaunch_failed");
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
    LOG_ERROR("neoweb watchdog: cannot start thread");
}

void watchdog_on_resume() {
  g_resume_due.store(kResumeCheckSeconds, std::memory_order_release);
}

} // namespace onion::daemon::neoweb
