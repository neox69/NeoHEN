/* NeoHEN: homebrew apps from GitHub releases, for NeoWEB's app store.
 *
 * A release zip holds one app folder, PPSA12345/ with eboot.bin and
 * sce_sys/param.json, at its top or one folder down (files beside it are
 * left out). util downloads it, unpacks it beside its final place
 * and swaps it into /data/homebrew, where ShadowMount+ registers it like any
 * game folder. One job at a time, in its own thread; NeoWEB asks how it goes.
 * A download that differs from the zip's SHA-256 is thrown away before
 * anything is unpacked. A SHA-256 is required: only a release file GitHub
 * publishes no digest for goes without one, and only when NeoWEB says the
 * user agreed ("unverified": true); apps.json remembers it as unverified.
 * Free space on /data is checked before the download (the zip's size) and
 * again before unpacking (what the zip says it holds).
 * What was installed, and which release, is kept in web/apps.json.
 *
 * "kind": "homebrew" installs a zip laid out for websrv's homebrew folder
 * instead (ps5-payload-dev/offact's OffAct.zip: OffAct.elf, homebrew.js,
 * sce_sys/icon0.png): its files, at the top of the zip or inside one folder,
 * go to /data/homebrew/<dir>/, a name NeoWEB's page gives ("OffAct"), and
 * NeoWEB's Homebrew section lists it. It must hold homebrew.js or eboot.elf.
 * Same download, SHA-256, free-space and zip-slip rules, the same swap by
 * rename; apps.json keeps it under "homebrew" instead of "installed". */

#include "app_store.hpp"

#include "cheats/sync/http_transport_ps5.hpp"
#include "onion_cjson.hpp"

#include <onion/brand.h>
#include <onion/fs.h>
#include <onion/log.h>
#include <onion/tree.h>

#include <miniz.h>
extern "C" {
#include <sha256.h>
}

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace {

/* /data is /user/data; the same mount as kWork, so the swap is a rename */
constexpr const char *kHome = "/user/data/homebrew";
constexpr const char *kWork = ONION_USER_DATA_DIR "/apps";
constexpr const char *kList = ONION_USER_DATA_DIR "/web/apps.json";
constexpr size_t kMaxZipBytes = 4ull << 30;
constexpr mz_uint64 kMaxAppBytes = 12ull << 30;
constexpr mz_uint kMaxEntries = 60000;
/* left free on /data on top of what an install needs */
constexpr unsigned long long kSpareBytes = 256ull << 20;

struct Job {
  std::string state = "idle"; /* idle download unpack done failed */
  std::string id, tag, url, sha, err; /* id: the title id, or the homebrew folder */
  bool homebrew = false;
  size_t got = 0, total = 0;
  bool unverified = false;
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

/* a folder name for /data/homebrew: letters, digits, "._-", not hidden, and
 * not a title id (those folders are ShadowMount+'s apps) */
bool dir_ok(const std::string &d) {
  if (d.empty() || d.size() > 64 || d[0] == '.' || id_ok(d))
    return false;
  for (char c : d)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-')
      return false;
  return true;
}

bool sha_ok(const std::string &s) {
  if (s.size() != 64)
    return false;
  for (char c : s)
    if (!std::isdigit(static_cast<unsigned char>(c)) && (c < 'a' || c > 'f'))
      return false;
  return true;
}

std::string hex(const uint8_t *d, size_t n) {
  static const char k[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += k[d[i] >> 4];
    s += k[d[i] & 15];
  }
  return s;
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

/* bytes free for us on the file system that holds path; ~0 when unknown */
unsigned long long free_bytes(const char *path) {
  struct statvfs st {};
  if (statvfs(path, &st) != 0)
    return ~0ull;
  return static_cast<unsigned long long>(st.f_bavail) *
         static_cast<unsigned long long>(st.f_frsize);
}

std::string gib(unsigned long long n) {
  char b[32];
  snprintf(b, sizeof(b), "%.1f GB", static_cast<double>(n) / (1024.0 * 1024.0 * 1024.0));
  return b;
}

/* "" when there is room for need bytes (plus a margin) where path lives */
std::string room_for(const char *path, unsigned long long need) {
  const unsigned long long have = free_bytes(path);
  if (have == ~0ull || have >= need + kSpareBytes)
    return "";
  return "not enough free space: " + gib(need + kSpareBytes) + " needed, " + gib(have) + " free";
}

void remember(const char *list, const std::string &id, const std::string &tag,
              const std::string &url, bool unverified) {
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
  cJSON *installed = cJSON_GetObjectItemCaseSensitive(root, list);
  if (!cJSON_IsObject(installed)) {
    cJSON_DeleteItemFromObjectCaseSensitive(root, list);
    installed = cJSON_AddObjectToObject(root, list);
  }
  cJSON *one = cJSON_CreateObject();
  cJSON_AddStringToObject(one, "tag", tag.c_str());
  cJSON_AddStringToObject(one, "url", url.c_str());
  cJSON_AddNumberToObject(one, "time", static_cast<double>(time(nullptr)));
  if (unverified)
    cJSON_AddBoolToObject(one, "unverified", true);
  cJSON_DeleteItemFromObjectCaseSensitive(installed, id.c_str());
  cJSON_AddItemToObject(installed, id.c_str(), one);
  char *out = cJSON_PrintUnformatted(root);
  if (out) {
    write_file_atomic(kList, out, strlen(out));
    free(out);
  }
  cJSON_Delete(root);
}

/* Where the app folder sits in the zip: "<id>/" at the top, or one folder
 * down ("PS5SX2-1.0/<id>/"), as some releases pack it beside their license
 * files. "" when the zip holds no "<id>/eboot.bin" at either place. */
std::string app_prefix(mz_zip_archive *zip, mz_uint n, const std::string &id) {
  const std::string tail = id + "/eboot.bin";
  std::string found;
  for (mz_uint i = 0; i < n; ++i) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(zip, i, &st) || !safe_path(st.m_filename))
      continue;
    const std::string name(st.m_filename);
    if (name.size() < tail.size() || name.compare(name.size() - tail.size(), tail.size(), tail) != 0)
      continue;
    const std::string above = name.substr(0, name.size() - tail.size());
    if (!above.empty() && (above.back() != '/' || above.find('/') != above.size() - 1))
      continue; /* deeper than one folder, or "XPPSA12345/" */
    if (!found.empty() && found != above + id + "/")
      return ""; /* two app folders: not one app */
    found = above + id + "/";
  }
  return found;
}

/* A homebrew folder zip: homebrew.js or eboot.elf at the top (prefix ""),
 * or every file inside one folder that has one of them ("OffAct/").
 * False when neither. */
bool homebrew_prefix(mz_zip_archive *zip, mz_uint n, std::string &prefix) {
  bool top = false, one = true;
  std::string first;
  for (mz_uint i = 0; i < n; ++i) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(zip, i, &st) || !safe_path(st.m_filename))
      continue; /* refused later */
    const std::string name(st.m_filename);
    if (name == "homebrew.js" || name == "eboot.elf")
      top = true;
    const size_t slash = name.find('/');
    const std::string folder = slash == std::string::npos ? "" : name.substr(0, slash + 1);
    if (folder.empty() || (!first.empty() && folder != first))
      one = false;
    else
      first = folder;
  }
  if (top) {
    prefix.clear();
    return true;
  }
  if (!one || first.empty())
    return false;
  for (mz_uint i = 0; i < n; ++i) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(zip, i, &st))
      continue;
    const std::string name(st.m_filename);
    if (name == first + "homebrew.js" || name == first + "eboot.elf") {
      prefix = first;
      return true;
    }
  }
  return false;
}

/* Unpack zip into stage: the "<id>/" folder only, wherever app_prefix found
 * it. Files beside it (a readme, the license) are left out. A homebrew zip:
 * everything, or what is inside its one folder. */
std::string unpack(const std::string &zip_path, const std::string &id, bool homebrew,
                   const std::string &stage) {
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0))
    return "not a zip file";
  std::string err;
  const mz_uint n = mz_zip_reader_get_num_files(&zip);
  bool eboot = false, param = false;
  mz_uint64 total = 0;
  if (n == 0 || n > kMaxEntries)
    err = "too many or no files in the zip";
  std::string prefix;
  if (err.empty() && homebrew && !homebrew_prefix(&zip, n, prefix))
    err = "the zip holds no homebrew.js or eboot.elf (at its top or in one folder)";
  if (err.empty() && !homebrew) {
    prefix = app_prefix(&zip, n, id);
    if (prefix.empty())
      err = "the zip holds no " + id + " folder with an eboot.bin";
  }
  for (mz_uint i = 0; err.empty() && i < n; ++i) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&zip, i, &st)) {
      err = "unreadable zip entry";
      break;
    }
    const std::string name(st.m_filename);
    if (!safe_path(st.m_filename)) {
      err = "the zip has an unsafe path";
      break;
    }
    if (name.compare(0, prefix.size(), prefix) != 0)
      continue; /* beside the app folder */
    eboot = eboot || name == prefix + "eboot.bin";
    param = param || name == prefix + "sce_sys/param.json";
    total += st.m_uncomp_size;
  }
  if (err.empty() && !homebrew && (!eboot || !param))
    err = "no eboot.bin or sce_sys/param.json in " + id;
  if (err.empty() && total > kMaxAppBytes)
    err = "the app is too big";
  /* the zip is still there while unpacking, so the whole app must fit */
  if (err.empty())
    err = room_for(kWork, static_cast<unsigned long long>(total));
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
    const std::string name(st.m_filename);
    if (name.compare(0, prefix.size(), prefix) != 0)
      continue;
    const std::string rel(name.substr(prefix.size()));
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
  /* "hb-": a homebrew folder never shares work names with a title id */
  const std::string tmp = (j.homebrew ? "hb-" : "") + j.id;
  const std::string zip = std::string(kWork) + "/" + tmp + ".zip";
  const std::string stage = std::string(kWork) + "/stage-" + tmp;
  const std::string old = std::string(kWork) + "/old-" + tmp;
  const std::string dest = std::string(kHome) + "/" + j.id;
  mkdir_tree(kWork);
  mkdir_tree(kHome);
  rmtree(stage.c_str());

  using namespace onion::cheats::sync;
  FILE *out = fopen(zip.c_str(), "wb");
  SHA256_CTX sha;
  sha256_init(&sha);
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
      sha256_update(&sha, static_cast<const uint8_t *>(data), n);
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
  uint8_t digest[SHA256_BLOCK_SIZE];
  sha256_final(&sha, digest);
  if (j.sha.empty()) {
    LOG_WARN("[apps] %s: no SHA-256 published, installed unverified (user agreed)", j.id.c_str());
  } else if (hex(digest, sizeof(digest)) != j.sha) {
    unlink(zip.c_str());
    LOG_ERROR("[apps] %s: SHA-256 mismatch, the download was thrown away", j.id.c_str());
    set("failed", "the download does not match the catalog's SHA-256");
    return nullptr;
  }

  set("unpack");
  std::string err = unpack(zip, j.id, j.homebrew, stage);
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
  remember(j.homebrew ? "homebrew" : "installed", j.id, j.tag, j.url, j.unverified);
  LOG_INFO("[apps] %s %s installed in %s", j.id.c_str(), j.tag.c_str(), dest.c_str());
  set("done");
  return nullptr;
}

std::string status_json() {
  std::lock_guard<std::mutex> lock(g_mu);
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "state", g_job.state.c_str());
  cJSON_AddStringToObject(o, "id", g_job.id.c_str());
  if (g_job.homebrew)
    cJSON_AddStringToObject(o, "kind", "homebrew");
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
  const bool homebrew = std::string(onion_cjson::string_item(request, "kind", "app")) == "homebrew";
  const std::string id =
      onion_cjson::string_item(request, homebrew ? "dir" : "id", "");
  const std::string tag = onion_cjson::string_item(request, "tag", "");
  const std::string sha = onion_cjson::string_item(request, "sha256", "");
  const cJSON *unv = cJSON_GetObjectItemCaseSensitive(request, "unverified");
  const bool unverified = sha.empty() && cJSON_IsTrue(unv);
  const cJSON *sz = cJSON_GetObjectItemCaseSensitive(request, "size");
  const unsigned long long size =
      cJSON_IsNumber(sz) && sz->valuedouble > 0 ? static_cast<unsigned long long>(sz->valuedouble) : 0;
  if (!(homebrew ? dir_ok(id) : id_ok(id)) || tag.size() > 60 || (!sha.empty() && !sha_ok(sha)) || url.compare(0, 19, "https://github.com/") != 0 ||
      url.size() > 400) {
    out = "rejected";
    return false;
  }
  /* decision 6.1 of the 1.0 plan: no SHA-256, no install, unless the user
   * said yes to an unverified one */
  if (sha.empty() && !unverified) {
    out = "no SHA-256 for this file (install it as unverified from NeoWEB)";
    return false;
  }
  if (size > kMaxZipBytes) {
    out = "the zip is too big";
    return false;
  }
  mkdir_tree(kWork);
  /* the zip itself; what it unpacks to is checked once it is here */
  const std::string room = room_for(kWork, size);
  if (!room.empty()) {
    LOG_ERROR("[apps] %s: %s", id.c_str(), room.c_str());
    out = room;
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
    g_job.homebrew = homebrew;
    g_job.tag = tag;
    g_job.url = url;
    g_job.sha = sha;
    g_job.unverified = unverified;
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
