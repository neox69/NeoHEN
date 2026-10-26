/* NeoHEN: homebrew apps from GitHub releases, for NeoWEB's app store.
 *
 * A release zip holds one app folder, PPSA12345/ with eboot.bin and
 * sce_sys/param.json. util downloads it, unpacks it beside its final place
 * and swaps it into /data/homebrew, where ShadowMount+ registers it like any
 * game folder. One job at a time, in its own thread; NeoWEB asks how it goes.
 * What was installed, and which release, is kept in web/apps.json. */

#include "app_store.hpp"

#include "cheats/sync/http_transport_ps5.hpp"
#include "onion_cjson.hpp"

#include <onion/brand.h>
#include <onion/fs.h>
#include <onion/log.h>
#include <onion/tree.h>

#include <miniz.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

/* /data is /user/data; the same mount as kWork, so the swap is a rename */
constexpr const char *kHome = "/user/data/homebrew";
constexpr const char *kWork = ONION_USER_DATA_DIR "/apps";
constexpr const char *kList = ONION_USER_DATA_DIR "/web/apps.json";
constexpr size_t kMaxZipBytes = 4ull << 30;
constexpr mz_uint64 kMaxAppBytes = 12ull << 30;
constexpr mz_uint kMaxEntries = 60000;

struct Job {
  std::string state = "idle"; /* idle download unpack done failed */
  std::string id, tag, url, err;
  size_t got = 0, total = 0;
};

std::mutex g_mu;
Job g_job;
std::atomic_bool g_cancel{false};

bool id_ok(const std::string &id) {
  if (id.size() != 9)
    return false;
  for (size_t i = 0; i < 9; ++i)
    if (i < 4 ? !std::isupper(static_cast<unsigned char>(id[i]))
              : !std::isdigit(static_cast<unsigned char>(id[i])))
      return false;
  return true;
}

/* no absolute paths, no "..", no backslashes, no empty parts */
bool safe_path(const char *p) {
  const std::string s(p);
  if (s.empty() || s.size() > 900 || s[0] == '/' || s.find('\\') != std::string::npos)
    return false;
  size_t start = 0;
  while (start <= s.size()) {
    const size_t slash = s.find('/', start);
    const std::string part =
        s.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (part == "." || part == ".." || (part.empty() && slash != std::string::npos &&
                                        slash + 1 < s.size()))
      return false;
    if (slash == std::string::npos)
      break;
    start = slash + 1;
  }
  return true;
}

void set(const char *state, const std::string &err = "") {
  std::lock_guard<std::mutex> lock(g_mu);
  g_job.state = state;
  g_job.err = err;
}

void progress(size_t received, size_t total, void *) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_job.got = received;
  g_job.total = total;
}

bool cancelled(void *) { return g_cancel.load(); }

void remember(const std::string &id, const std::string &tag, const std::string &url) {
  char *text = nullptr;
  size_t len = 0;
  cJSON *root = nullptr;
  if (read_file_alloc_str(kList, 1 << 20, &text, &len)) {
    root = cJSON_Parse(text);
    free(text);
  }
  if (!root || !cJSON_IsObject(root)) {
    cJSON_Delete(root);
    root = cJSON_CreateObject();
  }
  cJSON *installed = cJSON_GetObjectItemCaseSensitive(root, "installed");
  if (!cJSON_IsObject(installed))
    installed = cJSON_AddObjectToObject(root, "installed");
  cJSON *one = cJSON_CreateObject();
  cJSON_AddStringToObject(one, "tag", tag.c_str());
  cJSON_AddStringToObject(one, "url", url.c_str());
  cJSON_AddNumberToObject(one, "time", static_cast<double>(time(nullptr)));
  cJSON_DeleteItemFromObjectCaseSensitive(installed, id.c_str());
  cJSON_AddItemToObject(installed, id.c_str(), one);
  char *out = cJSON_PrintUnformatted(root);
  if (out) {
    write_file_atomic(kList, out, strlen(out));
    free(out);
  }
  cJSON_Delete(root);
}

/* Unpack zip into stage; every entry must sit under "<id>/". */
std::string unpack(const std::string &zip_path, const std::string &id,
                   const std::string &stage) {
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0))
    return "not a zip file";
  std::string err;
  const mz_uint n = mz_zip_reader_get_num_files(&zip);
  const std::string prefix = id + "/";
  bool eboot = false, param = false;
  mz_uint64 total = 0;
  if (n == 0 || n > kMaxEntries)
    err = "too many or no files in the zip";
  for (mz_uint i = 0; err.empty() && i < n; ++i) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&zip, i, &st)) {
      err = "unreadable zip entry";
      break;
    }
    const std::string name(st.m_filename);
    if (!safe_path(st.m_filename) || name.compare(0, prefix.size(), prefix) != 0) {
      err = "the zip is not one " + id + " folder";
      break;
    }
    eboot = eboot || name == prefix + "eboot.bin";
    param = param || name == prefix + "sce_sys/param.json";
    total += st.m_uncomp_size;
  }
  if (err.empty() && (!eboot || !param))
    err = "no eboot.bin or sce_sys/param.json in " + id;
  if (err.empty() && total > kMaxAppBytes)
    err = "the app is too big";
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_job.got = 0;
    g_job.total = static_cast<size_t>(total);
  }
  mz_uint64 done = 0;
  for (mz_uint i = 0; err.empty() && i < n; ++i) {
    if (g_cancel.load()) {
      err = "cancelled";
      break;
    }
    mz_zip_archive_file_stat st;
    mz_zip_reader_file_stat(&zip, i, &st);
    const std::string rel(st.m_filename + prefix.size());
    if (rel.empty())
      continue;
    const std::string dest = stage + "/" + rel;
    if (mz_zip_reader_is_file_a_directory(&zip, i)) {
      if (!mkdir_tree(dest.c_str()))
        err = "cannot write " + rel;
      continue;
    }
    const size_t cut = dest.rfind('/');
    if (!mkdir_tree(dest.substr(0, cut).c_str()) ||
        !mz_zip_reader_extract_to_file(&zip, i, dest.c_str(), 0)) {
      err = "cannot unpack " + rel + " (is the drive full?)";
      break;
    }
    chmod(dest.c_str(), 0777);
    done += st.m_uncomp_size;
    std::lock_guard<std::mutex> lock(g_mu);
    g_job.got = static_cast<size_t>(done);
  }
  mz_zip_reader_end(&zip);
  return err;
}

void *job_thread(void *) {
  Job j;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    j = g_job;
  }
  const std::string zip = std::string(kWork) + "/" + j.id + ".zip";
  const std::string stage = std::string(kWork) + "/stage-" + j.id;
  const std::string old = std::string(kWork) + "/old-" + j.id;
  const std::string dest = std::string(kHome) + "/" + j.id;
  mkdir_tree(kWork);
  mkdir_tree(kHome);
  rmtree(stage.c_str());

  using namespace onion::cheats::sync;
  FILE *out = fopen(zip.c_str(), "wb");
  SyncStatus st = out ? SyncStatus::Ok : SyncStatus::Io;
  if (out) {
    HttpRequest request;
    request.url = j.url.c_str();
    request.host_allow = "github.com";
    request.accept = "application/octet-stream";
    request.max_body_bytes = kMaxZipBytes;
    request.on_progress = progress;
    request.should_cancel = cancelled;
    Ps5HttpTransport http;
    st = http.perform(request, [&](const void *data, size_t n) {
      return fwrite(data, 1, n, out) == n ? SyncStatus::Ok : SyncStatus::Io;
    });
    if (fclose(out) != 0 && st == SyncStatus::Ok)
      st = SyncStatus::Io;
  }
  if (st != SyncStatus::Ok) {
    unlink(zip.c_str());
    LOG_ERROR("[apps] %s: download failed: %s", j.id.c_str(), sync_status_name(st));
    set("failed", g_cancel.load() ? "cancelled" : std::string("download: ") + sync_status_name(st));
    return nullptr;
  }

  set("unpack");
  std::string err = unpack(zip, j.id, stage);
  unlink(zip.c_str());
  if (err.empty()) {
    /* the new folder replaces the old one in one rename each */
    rmtree(old.c_str());
    if (if_exists(dest.c_str()) && rename(dest.c_str(), old.c_str()) != 0)
      err = "cannot move the installed version aside";
    else if (rename(stage.c_str(), dest.c_str()) != 0) {
      rename(old.c_str(), dest.c_str());
      err = "cannot put the new version in place";
    }
  }
  rmtree(stage.c_str());
  rmtree(old.c_str());
  if (!err.empty()) {
    LOG_ERROR("[apps] %s: %s", j.id.c_str(), err.c_str());
    set("failed", err);
    return nullptr;
  }
  remember(j.id, j.tag, j.url);
  LOG_INFO("[apps] %s %s installed in %s", j.id.c_str(), j.tag.c_str(), dest.c_str());
  set("done");
  return nullptr;
}

std::string status_json() {
  std::lock_guard<std::mutex> lock(g_mu);
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "state", g_job.state.c_str());
  cJSON_AddStringToObject(o, "id", g_job.id.c_str());
  cJSON_AddStringToObject(o, "tag", g_job.tag.c_str());
  cJSON_AddNumberToObject(o, "got", static_cast<double>(g_job.got));
  cJSON_AddNumberToObject(o, "total", static_cast<double>(g_job.total));
  cJSON_AddStringToObject(o, "err", g_job.err.substr(0, 90).c_str());
  char *text = cJSON_PrintUnformatted(o);
  std::string s = text ? text : "{}";
  free(text);
  cJSON_Delete(o);
  return s;
}

} // namespace

bool util_app_store(const cJSON *request, std::string &out) {
  const std::string action = onion_cjson::string_item(request, "action", "status");
  if (action == "status") {
    out = status_json();
    return true;
  }
  if (action == "cancel") {
    g_cancel.store(true);
    out = status_json();
    return true;
  }
  if (action != "install") {
    out = "unknown action";
    return false;
  }
  const std::string url = onion_cjson::string_item(request, "url", "");
  const std::string id = onion_cjson::string_item(request, "id", "");
  const std::string tag = onion_cjson::string_item(request, "tag", "");
  if (!id_ok(id) || tag.size() > 60 || url.compare(0, 19, "https://github.com/") != 0 ||
      url.size() > 400) {
    out = "rejected";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_job.state == "download" || g_job.state == "unpack") {
      out = "another app is being installed";
      return false;
    }
    g_job = Job();
    g_job.state = "download";
    g_job.id = id;
    g_job.tag = tag;
    g_job.url = url;
  }
  g_cancel.store(false);
  pthread_t thread{};
  if (pthread_create(&thread, nullptr, job_thread, nullptr) != 0) {
    set("failed", "cannot start");
    out = "cannot start";
    return false;
  }
  pthread_detach(thread);
  LOG_INFO("[apps] %s %s <- %s", id.c_str(), tag.c_str(), url.c_str());
  out = status_json();
  return true;
}
