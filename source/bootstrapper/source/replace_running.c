/* NeoHEN: a NeoHEN that is already running is replaced, not doubled.
 *
 * Sending NeoHEN.elf while an older (or the same) NeoHEN is loaded used to
 * start a second daemon and inject a second Toolbox into the same home
 * screen, which hangs the console. Now the bootstrapper asks the running
 * daemon to shut its stack down (the same command neohen_ctl.py sends on TCP
 * 9048: util and the private loader stop, SceShellUI restarts so the old
 * Toolbox leaves memory, the daemon exits) and waits for each of those to
 * have really happened before it goes on. If the old daemon does not answer,
 * the same steps are done here. kstuff is never touched: launch_kstuff()
 * already skips it when it is running. */
#include "replace_running.h"

#include "bootstrap_notify.h"

#include <onion/log.h>
#include <onion/proc_query.h>
#include <onion/ready.h>
#include <onion/system_tmp.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

/* Same wire as include/msg.hpp (ONION_CTRL_TCP_*). */
#define CTRL_PORT 9048
#define CTRL_MAGIC 0x4F4E494Fu
#define CTRL_CMD_SHUTDOWN 1u

#define DAEMON_NAME "onion_daemon.elf"
#define UTIL_NAME "onion_util.elf"
#define LOADER_NAME "onion_elfldr.elf"

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool ask_shutdown(void) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return false;
  struct timeval tv = {2, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(CTRL_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool ok = false;
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
    const uint32_t frame[2] = {CTRL_MAGIC, CTRL_CMD_SHUTDOWN};
    ok = send(fd, frame, sizeof(frame), 0) == (ssize_t)sizeof(frame);
    unsigned char reply = 0xff;
    (void)recv(fd, &reply, 1, 0); /* the daemon may exit before replying */
  }
  close(fd);
  return ok;
}

static bool wait_gone(const char *name, int timeout_ms) {
  const uint64_t until = now_ms() + (uint64_t)timeout_ms;
  while (now_ms() < until) {
    if (onion_find_pid(name) <= 0)
      return true;
    usleep(100 * 1000);
  }
  return onion_find_pid(name) <= 0;
}

static void kill_named(const char *name) {
  for (int i = 0; i < 4; ++i) {
    const pid_t pid = onion_find_pid(name);
    if (pid <= 1 || pid == getpid())
      return;
    LOG_INFO("replace: stopping %s pid=%d", name, (int)pid);
    if (kill(pid, SIGKILL) != 0 && errno != ESRCH)
      return;
    usleep(150 * 1000);
  }
}

static bool shellui_ready(pid_t pid) {
  uint32_t handle = (uint32_t)-1;
  return pid > 1 && kernel_dynlib_handle(pid, "libSceNpTrophy.sprx", &handle) == 0 &&
         handle != (uint32_t)-1;
}

/* The home screen that held the old Toolbox is gone, and a new one is up
 * with its libraries mapped (the point at which the new daemon may inject). */
static bool wait_new_shellui(pid_t old_pid, int timeout_ms) {
  const uint64_t until = now_ms() + (uint64_t)timeout_ms;
  while (now_ms() < until) {
    const pid_t now = onion_find_pid("SceShellUI");
    if (now > 1 && now != old_pid && shellui_ready(now))
      return true;
    usleep(200 * 1000);
  }
  return false;
}

int sceSystemServiceGetAppIdOfRunningBigApp(void);

bool bootstrap_replace_running_neohen(void) {
  const pid_t daemon = onion_find_pid(DAEMON_NAME);
  if (daemon <= 1)
    return true; /* nothing loaded: the usual path */

  /* Taking the old Toolbox out means restarting the home screen: not with a
   * game open. The running NeoHEN stays as it is. */
  if (sceSystemServiceGetAppIdOfRunningBigApp() > 0) {
    LOG_WARN("replace: a game is open; the running NeoHEN is kept");
    bootstrap_notify("notify.boot.replace_game_open");
    return false;
  }

  const pid_t old_shellui = onion_find_pid("SceShellUI");
  LOG_INFO("replace: a NeoHEN is already running (daemon pid=%d): stopping it",
           (int)daemon);
  bootstrap_notify("notify.boot.replacing");

  const uint64_t t0 = now_ms();
  const bool asked = ask_shutdown();
  if (!asked || !wait_gone(DAEMON_NAME, 8000)) {
    /* Too old to know the command, or stuck: do its steps from here. */
    LOG_WARN("replace: the old daemon did not stop by itself (%s); stopping it here",
             asked ? "timeout" : "no answer on 9048");
    kill_named(UTIL_NAME);
    kill_named(LOADER_NAME);
    kill_named(DAEMON_NAME);
    if (old_shellui > 1 && onion_find_pid("SceShellUI") == old_shellui) {
      LOG_INFO("replace: restarting SceShellUI pid=%d", (int)old_shellui);
      (void)kill(old_shellui, SIGKILL);
    }
  }
  /* Each step, as it really ends, not a fixed wait. */
  const bool util_gone = wait_gone(UTIL_NAME, 5000);
  const bool loader_gone = wait_gone(LOADER_NAME, 5000);
  const bool daemon_gone = wait_gone(DAEMON_NAME, 5000);
  const bool menu_back = wait_new_shellui(old_shellui, 30000);
  onion_ready_clear(ONION_READY_DAEMON);
  onion_ready_clear(ONION_READY_UTIL);
  onion_ready_clear(ONION_READY_TOOLBOX);
  unlink(ONION_SYSTEM_TMP_ELFLDR_STATE);
  unlink(ONION_SYSTEM_TMP_ELFLDR_BUSY);
  LOG_INFO("replace: old stack down in %llu ms (util=%d loader=%d daemon=%d menu=%d)",
           (unsigned long long)(now_ms() - t0), util_gone, loader_gone, daemon_gone,
           menu_back);
  if (!daemon_gone) {
    bootstrap_notify("notify.boot.replace_failed");
    return false;
  }
  return true;
}
