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

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

void *load_payload_thread(void *args);

namespace {

constexpr const char *kSmpElf = ONION_USER_DATA_DIR "/payloads/shadowmountplus.elf";
/* SceShellUI does not see every path the way a payload does (/data is
 * /user/data, a USB drive may be /usb0), so each place is tried both ways. */
constexpr const char *kSmpLogs[] = {"/user/data/shadowmount/debug.log",
                                    "/data/shadowmount/debug.log"};
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

std::string read_small(const std::string &path, size_t limit = 4096) {
  std::string out;
  FILE *f = fopen(path.c_str(), "rb");
  if (!f)
    return out;
  out.resize(limit);
  out.resize(fread(out.data(), 1, limit, f));
  fclose(f);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == '\0'))
    out.pop_back();
  return out;
}

/* The value of "key":"value" in a small JSON text; enough for param.json. */
std::string json_string(const std::string &text, const char *key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t at = text.find(needle);
  if (at == std::string::npos)
    return {};
  at = text.find(':', at + needle.size());
  if (at == std::string::npos)
    return {};
  const size_t open_quote = text.find('"', at);
  if (open_quote == std::string::npos)
    return {};
  const size_t close_quote = text.find('"', open_quote + 1);
  if (close_quote == std::string::npos)
    return {};
  return text.substr(open_quote + 1, close_quote - open_quote - 1);
}

std::string size_text(off_t bytes) {
  char out[32];
  if (bytes >= (1LL << 30))
    snprintf(out, sizeof(out), "%.1f GB", static_cast<double>(bytes) / (1LL << 30));
  else
    snprintf(out, sizeof(out), "%lld MB", static_cast<long long>(bytes >> 20));
  return out;
}

bool game_is_open(const std::string &title_id) {
  std::string running;
  int app_id = 0;
  return Get_Running_App_TID(running, app_id) && running == title_id;
}

/* ---------------------------------------------------------------- info */

void action_info(const std::string &tid) {
  const std::string param = read_small("/user/appmeta/" + tid + "/param.json", 256 * 1024);
  std::string version = json_string(param, "contentVersion");
  if (version.empty())
    version = "?";
  std::string source = read_small("/user/app/" + tid + "/mount_img.lnk");
  if (source.empty())
    source = read_small("/user/app/" + tid + "/mount.lnk");
  if (source.empty())
    source = "/user/app/" + tid;
  struct stat st {};
  bool found = stat(source.c_str(), &st) == 0;
  if (!found && source.rfind("/data/", 0) == 0)
    found = stat(("/user" + source).c_str(), &st) == 0;
  const std::string size = found && S_ISREG(st.st_mode) ? size_text(st.st_size)
                                                        : std::string("-");
  notify("notify.neo.info", tid.c_str(), version.c_str(), source.c_str(), size.c_str());
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

/* The newest backup folder of the game, in the USB or on the console. */
std::string latest_backup(const std::string &tid) {
  std::string best_name, best_path;
  std::vector<std::string> roots;
  for (const char *usb : kUsbRoots)
    roots.push_back(std::string(usb) + kSavesFolder);
  roots.push_back(kLocalSaves);
  for (const std::string &root : roots) {
    const std::string base = root + "/" + tid;
    DIR *dir = opendir(base.c_str());
    if (!dir)
      continue;
    while (const dirent *e = readdir(dir)) {
      /* Folders made before a restore are a safety net, not a backup. */
      if (e->d_name[0] == '.' || std::strstr(e->d_name, "-pre"))
        continue;
      if (best_name.empty() || best_name < e->d_name) {
        best_name = e->d_name;
        best_path = base + "/" + e->d_name;
      }
    }
    closedir(dir);
  }
  return best_path;
}

void action_save_restore(const std::string &tid) {
  if (game_is_open(tid)) {
    notify("notify.neo.game_open", tid.c_str());
    return;
  }
  const std::string from = latest_backup(tid);
  if (from.empty()) {
    notify("notify.neo.restore_none", tid.c_str());
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
  else if (job->uri == kNeoActionInfo)
    action_info(job->tid);
  else if (job->uri == kNeoActionBackport)
    action_backport(job->tid);
  else if (job->uri == kNeoActionSaveBackup)
    action_save_backup(job->tid);
  else if (job->uri == kNeoActionSaveRestore)
    action_save_restore(job->tid);
  delete job;
  return nullptr;
}

} // namespace

bool neo_game_action(const std::string &uri, const std::string &title_id) {
  if (uri.rfind(kNeoActionPrefix, 0) != 0)
    return false;
  Job *job = new Job{uri, title_id.empty() ? g_ui.current_menu_tid : title_id};
  LOG_INFO("game menu action %s for %s", uri.c_str(), job->tid.c_str());
  pthread_t thread;
  if (pthread_create(&thread, nullptr, action_thread, job) == 0)
    pthread_detach(thread);
  else
    delete job;
  return true;
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
