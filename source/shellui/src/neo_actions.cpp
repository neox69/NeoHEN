/* NeoHEN: ShadowMount+ relaunch from the Toolbox and the per-game actions of
 * the game options menu. See neo_actions.hpp. */
#include "neo_actions.hpp"

#include "hooked_funcs.hpp"
#include "ipc.hpp"
#include "shellui_payload_state.hpp"
#include "shellui_state.hpp"
#include "shellui_types.hpp"
#include "toolbox_i18n.hpp"
#include <onion/brand.h>
#include <onion/payload.h>
#include "ps5_settings_ui.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <msg.hpp>
#include <netinet/in.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <utility>
#include <unistd.h>
#include <vector>

void *load_payload_thread(void *args);

namespace {

constexpr const char *kSmpElf = ONION_USER_DATA_DIR "/payloads/shadowmountplus.elf";
/* SceShellUI does not see every path the way a payload does (/data is
 * /user/data, a USB drive may be /usb0), so each place is tried both ways. */
constexpr const char *kSmpLogs[] = {"/user/data/shadowmount/debug.log",
                                    "/data/shadowmount/debug.log"};
/* Not a path: the job that backs up instead of restoring one. */
constexpr const char *kSaveBackupJob = "backup";
constexpr long kSmpLogMax = 8L << 20;
constexpr const char *kUsbRoots[] = {"/mnt/usb0", "/usb0"};
constexpr const char *kSavesFolder = "/" ONION_BRAND_NAME "/saves";
constexpr const char *kLocalSaves = ONION_USER_DATA_DIR "/saves";
/* <scan path>/backports/<TITLE_ID> is ShadowMount+'s overlay folder. */
constexpr const char *kBackportRoots[] = {
    "/user/data/homebrew/backports", "/data/homebrew/backports",
    "/mnt/usb0/homebrew/backports",  "/usb0/homebrew/backports",
    "/mnt/usb0/backports",           "/usb0/backports",
};

bool is_dir(const std::string &path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool mkdirs(const std::string &path) {
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i != path.size() && path[i] != '/')
      continue;
    const std::string part = path.substr(0, i);
    if (mkdir(part.c_str(), 0777) != 0 && errno != EEXIST)
      return false;
  }
  return true;
}

bool copy_file(const std::string &from, const std::string &to) {
  const int in = open(from.c_str(), O_RDONLY);
  if (in < 0)
    return false;
  const int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (out < 0) {
    close(in);
    return false;
  }
  std::vector<char> buffer(1 << 20);
  bool ok = true;
  for (;;) {
    const ssize_t n = read(in, buffer.data(), buffer.size());
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      ok = n == 0;
      break;
    }
    ssize_t done = 0;
    while (done < n) {
      const ssize_t w = write(out, buffer.data() + done, static_cast<size_t>(n - done));
      if (w < 0 && errno == EINTR)
        continue;
      if (w <= 0) {
        ok = false;
        break;
      }
      done += w;
    }
    if (!ok)
      break;
  }
  close(in);
  if (close(out) != 0)
    ok = false;
  return ok;
}

/* Copies a folder; returns the number of files, or -1 on the first error. */
int copy_tree(const std::string &from, const std::string &to) {
  if (!mkdirs(to))
    return -1;
  DIR *dir = opendir(from.c_str());
  if (!dir)
    return -1;
  int files = 0;
  while (const dirent *e = readdir(dir)) {
    if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
      continue;
    const std::string src = from + "/" + e->d_name;
    const std::string dst = to + "/" + e->d_name;
    struct stat st {};
    if (stat(src.c_str(), &st) != 0) {
      files = -1;
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      const int n = copy_tree(src, dst);
      if (n < 0) {
        files = -1;
        break;
      }
      files += n;
    } else if (S_ISREG(st.st_mode)) {
      if (!copy_file(src, dst)) {
        files = -1;
        break;
      }
      ++files;
    }
  }
  closedir(dir);
  return files;
}

bool game_is_open(const std::string &title_id) {
  std::string running;
  int app_id = 0;
  return Get_Running_App_TID(running, app_id) && running == title_id;
}

/* ---------------------------------------------------------------- backport */

void action_backport(const std::string &tid) {
  for (const char *root : kBackportRoots) {
    const std::string on = std::string(root) + "/" + tid;
    const std::string off = on + ".off";
    if (is_dir(on)) {
      if (rename(on.c_str(), off.c_str()) == 0)
        notify("notify.neo.backport_off", tid.c_str());
      else
        notify("notify.neo.failed", tid.c_str(), strerror(errno));
      return;
    }
    if (is_dir(off)) {
      if (rename(off.c_str(), on.c_str()) == 0)
        notify("notify.neo.backport_on", tid.c_str());
      else
        notify("notify.neo.failed", tid.c_str(), strerror(errno));
      return;
    }
  }
  notify("notify.neo.backport_none", tid.c_str());
}

/* ---------------------------------------------------------------- saves */

struct SaveDir {
  std::string user; /* folder name under /user/home */
  std::string kind; /* savedata_prospero or savedata */
  std::string path;
};

std::vector<SaveDir> find_saves(const std::string &tid) {
  std::vector<SaveDir> out;
  DIR *home = opendir("/user/home");
  if (!home)
    return out;
  while (const dirent *e = readdir(home)) {
    if (e->d_name[0] == '.')
      continue;
    for (const char *kind : {"savedata_prospero", "savedata"}) {
      const std::string path = std::string("/user/home/") + e->d_name + "/" + kind + "/" + tid;
      if (is_dir(path))
        out.push_back({e->d_name, kind, path});
    }
  }
  closedir(home);
  return out;
}

std::string saves_root() {
  /* A backup is worth more off the console's own disk: prefer the USB. */
  for (const char *usb : kUsbRoots) {
    const std::string dir = std::string(usb) + kSavesFolder;
    if (is_dir(usb) && mkdirs(dir))
      return dir;
  }
  return kLocalSaves;
}

std::string stamp() {
  char out[32];
  const time_t now = time(nullptr);
  struct tm tm {};
  localtime_r(&now, &tm);
  strftime(out, sizeof(out), "%Y%m%d-%H%M%S", &tm);
  return out;
}

/* Copies every user's save of the game to <root>/<tid>/<name>. */
int backup_to(const std::string &tid, const std::string &dest) {
  int files = 0;
  for (const SaveDir &save : find_saves(tid)) {
    const int n = copy_tree(save.path, dest + "/" + save.user + "/" + save.kind);
    if (n < 0)
      return -1;
    files += n;
  }
  return files;
}

void action_save_backup(const std::string &tid) {
  if (find_saves(tid).empty()) {
    notify("notify.neo.save_none", tid.c_str());
    return;
  }
  const std::string dest = saves_root() + "/" + tid + "/" + stamp();
  const int files = backup_to(tid, dest);
  if (files < 0)
    notify("notify.neo.failed", tid.c_str(), strerror(errno));
  else
    notify("notify.neo.save_ok", tid.c_str(), files, dest.c_str());
}

struct Backup {
  std::string name; /* 20261007-160057, or the same with -pre */
  std::string path;
  bool usb;
};

/* Every backup of the game, newest first. */
std::vector<Backup> list_backups(const std::string &tid) {
  std::vector<Backup> out;
  std::vector<std::pair<std::string, bool>> roots;
  for (const char *usb : kUsbRoots)
    roots.push_back({std::string(usb) + kSavesFolder, true});
  roots.push_back({kLocalSaves, false});
  for (const auto &root : roots) {
    const std::string base = root.first + "/" + tid;
    DIR *dir = opendir(base.c_str());
    if (!dir)
      continue;
    while (const dirent *e = readdir(dir))
      if (e->d_name[0] != '.' && is_dir(base + "/" + e->d_name))
        out.push_back({e->d_name, base + "/" + e->d_name, root.second});
    closedir(dir);
  }
  std::sort(out.begin(), out.end(),
            [](const Backup &a, const Backup &b) { return a.name > b.name; });
  return out;
}

void action_save_restore(const std::string &tid, const std::string &from) {
  if (game_is_open(tid)) {
    notify("notify.neo.game_open", tid.c_str());
    return;
  }
  /* What is on the console now is kept first, whatever happens next. */
  if (!find_saves(tid).empty() &&
      backup_to(tid, saves_root() + "/" + tid + "/" + stamp() + "-pre") < 0) {
    notify("notify.neo.failed", tid.c_str(), strerror(errno));
    return;
  }
  int files = 0;
  DIR *users = opendir(from.c_str());
  if (users) {
    while (const dirent *u = readdir(users)) {
      if (u->d_name[0] == '.')
        continue;
      for (const char *kind : {"savedata_prospero", "savedata"}) {
        const std::string src = from + "/" + u->d_name + "/" + kind;
        if (!is_dir(src))
          continue;
        const int n = copy_tree(src, std::string("/user/home/") + u->d_name + "/" + kind + "/" + tid);
        if (n < 0) {
          closedir(users);
          notify("notify.neo.failed", tid.c_str(), strerror(errno));
          return;
        }
        files += n;
      }
    }
    closedir(users);
  }
  notify("notify.neo.restore_ok", tid.c_str(), files, from.c_str());
}

/* The save data page: which game it is for and what it listed. */
std::string g_saves_tid;
bool g_saves_pending = false;
std::vector<Backup> g_saves_listed;

/* 20261007-160057 -> 07/10/2026 16:00 */
std::string backup_date(const std::string &name) {
  if (name.size() < 15)
    return name;
  return name.substr(6, 2) + "/" + name.substr(4, 2) + "/" + name.substr(0, 4) +
         " " + name.substr(9, 2) + ":" + name.substr(11, 2);
}

/* util keeps a payload it believes is running, so the old ShadowMount+ is
 * stopped first; then the usual loader thread starts the new one. */
void *smp_relaunch_thread(void *arg) {
  PayloadEntry *entry = static_cast<PayloadEntry *>(arg);
  char pid_path[256];
  const int pid = static_cast<int>(shellui_payload_resolve_recorded_pid(
      entry->tid.c_str(), pid_path, sizeof(pid_path)));
  if (pid > 1) {
    LOG_INFO("relaunch: stopping ShadowMount+ pid %d", pid);
    IPC_Client::getInstance(false).ForceKillPID(pid);
    unlink(pid_path);
    sleep(1);
  }
  return load_payload_thread(entry);
}

struct Job {
  std::string uri;
  std::string tid;
};

void *action_thread(void *arg) {
  Job *job = static_cast<Job *>(arg);
  if (job->tid.empty())
    notify("notify.neo.no_title");
  else if (job->uri == kNeoActionBackport)
    action_backport(job->tid);
  else if (job->uri == kSaveBackupJob)
    action_save_backup(job->tid);
  else
    action_save_restore(job->tid, job->uri);
  delete job;
  return nullptr;
}

void run_job(const std::string &what, const std::string &tid) {
  Job *job = new Job{what, tid};
  pthread_t thread;
  if (pthread_create(&thread, nullptr, action_thread, job) == 0)
    pthread_detach(thread);
  else
    delete job;
}

} // namespace

bool neo_game_action(const std::string &uri, const std::string &title_id) {
  if (uri != kNeoActionBackport)
    return false;
  const std::string tid = title_id.empty() ? g_ui.current_menu_tid : title_id;
  LOG_INFO("game menu action %s for %s", uri.c_str(), tid.c_str());
  run_job(uri, tid);
  return true;
}

bool neo_saves_page_request(const std::string &uri, const std::string &title_id) {
  if (uri != kNeoActionSaves)
    return false;
  g_saves_tid = title_id.empty() ? g_ui.current_menu_tid : title_id;
  g_saves_pending = true;
  LOG_INFO("save data page for %s", g_saves_tid.c_str());
  return true;
}

bool neo_saves_page_pending() { return g_saves_pending; }

void neo_generate_saves_xml(std::string &xml) {
  g_saves_pending = false;
  const std::string &tid = g_saves_tid;
  /* Shown in place of the Toolbox root, so it must carry the root list's id
   * (as the cheats page does when a game's menu opens it); with any other
   * id SceShellUI crashes while it lays the page out. */
  ps5ui::Page page("id_debug_settings",
                   toolbox_i18n::format("saves.title_fmt", tid.c_str()));
  page.root_style(ps5ui::Style::Center);
  if (find_saves(tid).empty())
    page.label("id_save_none", toolbox_i18n::tr("saves.no_save"), ps5ui::Style::Center);
  else
    page.button("id_save_backup", toolbox_i18n::tr("saves.backup"),
                toolbox_i18n::format("saves.backup.desc_fmt", saves_root().c_str()),
                std::nullopt, std::nullopt, ps5ui::Style::Center);

  g_saves_listed = list_backups(tid);
  if (g_saves_listed.size() > static_cast<size_t>(kNeoSavesListed))
    g_saves_listed.resize(kNeoSavesListed);
  if (g_saves_listed.empty())
    page.label("id_save_empty", toolbox_i18n::tr("saves.none"), ps5ui::Style::Center);
  for (size_t i = 0; i < g_saves_listed.size(); ++i) {
    const Backup &b = g_saves_listed[i];
    std::string where = toolbox_i18n::tr(b.usb ? "saves.where_usb" : "saves.where_console");
    if (b.name.find("-pre") != std::string::npos)
      where += std::string(" · ") + toolbox_i18n::tr("saves.pre");
    page.button("id_save_restore_" + std::to_string(i),
                toolbox_i18n::format("saves.restore_fmt", backup_date(b.name).c_str()),
                where, std::nullopt, std::nullopt, ps5ui::Style::Center,
                toolbox_i18n::tr("saves.restore.confirm"),
                toolbox_i18n::tr("cheats.repo.download.confirm_phrase"));
  }
  xml = page.build();
}

void neo_saves_backup_now() { run_job(kSaveBackupJob, g_saves_tid); }

void neo_saves_restore(int index) {
  if (index < 0 || static_cast<size_t>(index) >= g_saves_listed.size())
    return;
  run_job(g_saves_listed[static_cast<size_t>(index)].path, g_saves_tid);
}
namespace {
constexpr const char *kThemesDir = ONION_USER_DATA_DIR "/themes";
std::vector<std::string> g_themes_listed;
} // namespace

const std::vector<std::string> &neo_themes_refresh() {
  g_themes_listed.clear();
  if (DIR *dir = opendir(kThemesDir)) {
    while (const dirent *e = readdir(dir)) {
      /* "." folders are hidden; "_" ones are work in progress */
      if (e->d_name[0] == '.' || e->d_name[0] == '_')
        continue;
      if (is_dir(std::string(kThemesDir) + "/" + e->d_name))
        g_themes_listed.push_back(e->d_name);
    }
    closedir(dir);
  }
  std::sort(g_themes_listed.begin(), g_themes_listed.end());
  if (g_themes_listed.size() > static_cast<size_t>(kNeoThemesListed))
    g_themes_listed.resize(kNeoThemesListed);
  return g_themes_listed;
}

std::string neo_theme_active() {
  std::string name;
  if (FILE *f = fopen((std::string(kThemesDir) + "/active.txt").c_str(), "rb")) {
    char line[160] = {};
    if (fgets(line, sizeof(line), f))
      name = line;
    fclose(f);
  }
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
    name.pop_back();
  return name;
}

void neo_theme_set(int index) {
  std::string name;
  if (index >= 0) {
    if (static_cast<size_t>(index) >= g_themes_listed.size())
      return;
    name = g_themes_listed[static_cast<size_t>(index)];
  }
  mkdirs(kThemesDir);
  FILE *f = fopen((std::string(kThemesDir) + "/active.txt").c_str(), "wb");
  if (!f) {
    notify("notify.neo.failed", name.empty() ? "-" : name.c_str(), strerror(errno));
    return;
  }
  if (!name.empty())
    fprintf(f, "%s\n", name.c_str());
  fclose(f);
  /* neotheme.elf sees the change within seconds and says when it is on. */
  notify("notify.neo.theme_wait");
}

namespace {
/* neotheme.elf rewrites themes/state.txt each time it has put a theme (or the
 * stock look) in place. The menu only reads backgrounds and music when it
 * needs them, so it is told to reload its home screen. */
void *theme_watch_thread(void *) {
  const std::string path = std::string(kThemesDir) + "/state.txt";
  struct stat last {};
  bool have = stat(path.c_str(), &last) == 0;
  for (;;) {
    sleep(2);
    struct stat now {};
    if (stat(path.c_str(), &now) != 0)
      continue;
    if (have && now.st_mtime == last.st_mtime && now.st_size == last.st_size)
      continue;
    const bool first = !have;
    have = true;
    last = now;
    if (first)
      continue;
    LOG_INFO("theme changed: reloading the home screen");
    shellui_request_home_reload();
  }
  return nullptr;
}
} // namespace

void neo_menu_restart() {
  const uint32_t frame[2] = {ONION_CTRL_TCP_MAGIC, ONION_CTRL_TCP_CMD_MENU};
  unsigned char reply = 0xff;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(ONION_CTRL_TCP_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  timeval tv{3, 0};
  const int s = socket(AF_INET, SOCK_STREAM, 0);
  bool sent = false;
  if (s >= 0) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sent = connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 &&
           send(s, frame, sizeof(frame), 0) == static_cast<ssize_t>(sizeof(frame)) &&
           recv(s, &reply, 1, 0) == 1;
    close(s);
  }
  LOG_INFO("menu restart asked: sent=%d reply=%u", sent ? 1 : 0, reply);
  if (!sent)
    notify("notify.neo.failed", "menu", strerror(errno));
  else if (reply == 2)
    notify("notify.neo.menu_busy");
}

void neo_theme_watch_start() {
  static bool started = false;
  if (started)
    return;
  started = true;
  pthread_t thread;
  if (pthread_create(&thread, nullptr, theme_watch_thread, nullptr) == 0)
    pthread_detach(thread);
}

void neo_smp_relaunch() {
  char identity[ONION_PAYLOAD_IDENTITY_SIZE];
  char canonical[ONION_PAYLOAD_PATH_SIZE];
  if (access(kSmpElf, R_OK) != 0 ||
      !onion_payload_identity_from_path(kSmpElf, identity, sizeof(identity)) ||
      !onion_payload_canonical_path(kSmpElf, canonical, sizeof(canonical))) {
    notify("notify.payload.missing_fmt", "shadowmountplus.elf", ONION_DATA_DIR "/payloads");
    return;
  }
  auto *entry = new PayloadEntry();
  entry->shellui_path = kSmpElf;
  entry->path = canonical;
  entry->tid = identity;
  entry->id = identity;
  entry->name = "shadowmountplus.elf";
  pthread_t thread;
  if (pthread_create(&thread, nullptr, smp_relaunch_thread, entry) == 0) {
    pthread_detach(thread);
  } else {
    delete entry;
    notify("notify.smp.relaunch_failed");
  }
}

std::string neo_smp_status() {
  std::string tail;
  FILE *f = nullptr;
  for (const char *path : kSmpLogs)
    if ((f = fopen(path, "rb")))
      break;
  if (f) {
    if (fseek(f, 0, SEEK_END) == 0) {
      /* The "hooks installed" line is written once, at start, so the whole
       * log is read (ShadowMount+ rotates it well below this cap). */
      const long size = ftell(f);
      const long start = size > kSmpLogMax ? size - kSmpLogMax : 0;
      if (fseek(f, start, SEEK_SET) == 0) {
        tail.resize(static_cast<size_t>(size - start));
        tail.resize(fread(tail.data(), 1, tail.size(), f));
      }
    }
    fclose(f);
  }
  /* Same lines the daemon's watchdog reads (ShadowMount+ 1.7). */
  const size_t installed = tail.rfind("[SHELLCORE] hooks installed:");
  const size_t lost = tail.rfind("AppInstallAll hook is no longer installed");
  if (installed == std::string::npos && lost == std::string::npos)
    return toolbox_i18n::tr("smp.status.unknown");
  if (lost != std::string::npos && (installed == std::string::npos || lost > installed))
    return toolbox_i18n::tr("smp.status.lost");
  return toolbox_i18n::tr("smp.status.ok");
}
