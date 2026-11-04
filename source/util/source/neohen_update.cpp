/* NeoHEN: update NeoHEN itself from NeoWEB, with a way back.
 *
 * NeoHEN.elf is sent to the exploit's loader on every boot; it is not
 * installed anywhere. So an update keeps the new release on the console
 * (/data/neohen/update/current) and the next NeoHEN.elf that starts hands
 * off to it when it is newer than itself (bootstrapper, bootstrap_update.c;
 * the shared rules are in onion/update_state.h). The NeoHEN.elf the user
 * sends stays the last fallback.
 *
 * check    GitHub's releases of neox69/NeoHEN, picked by update.channel the
 *          same way as the notice in update_check.cpp, into available.json
 *          (tag, date, release notes, the zip and its SHA-256) for NeoWEB;
 * install  that release's NeoHEN-<version>.zip: GitHub's SHA-256 digest is
 *          required and checked, free space on /data is checked before the
 *          download and before unpacking, the zip is unpacked into
 *          update/staging and checked (one NeoHEN-<version>/ folder with
 *          NeoHEN.elf, LEEME.txt and TERCEROS.txt, an x86-64 ELF, and the
 *          release marker inside NeoHEN.elf saying <version>), then swapped
 *          in by directory renames on the same mount: previous/ is dropped,
 *          current/ becomes previous/, staging/ becomes current/. One older
 *          release is kept, never more. state.json says "trial";
 * rollback previous/ back to current/ (or no current/ at all: the NeoHEN.elf
 *          that is sent runs);
 * apply    the installed release now, the way an update was always applied:
 *          its NeoHEN.elf is sent to 9021 and replaces the running NeoHEN
 *          (replace_running.c; not with a game open).
 *
 * A trial release becomes "ok" when one of its starts reaches boot OK; two
 * starts that do not, and the bootstrapper rolls it back by itself.
 * One job (check or install) at a time, in its own thread; NeoWEB polls. */
#include "neohen_update.hpp"

#include "cheats/sync/http_transport_ps5.hpp"
#include "onion_cjson.hpp"

#include <elfldr_remote.h>
#include <onion/brand.h>
#include <onion/fs.h>
#include <onion/log.h>
#include <onion/notify.h>
#include <onion/settings.hpp>
#include <onion/tree.h>
#include <onion/update_state.h>
#include <onion/version.h>

#include <miniz.h>
extern "C" {
#include <sha256.h>
int sceSystemServiceGetAppIdOfRunningBigApp(void);
}

#include <algorithm>
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

extern onion::SettingsStore g_settings;

namespace {

constexpr const char *kApi = "https://api.github.com/repos/neox69/NeoHEN";
constexpr const char *kDownloads = "https://github.com/neox69/NeoHEN/releases/download/";
const std::string kDir = ONION_USER_DATA_DIR ONION_UPDATE_SUBDIR;
const std::string kCurrent = kDir + "/current";
const std::string kPrevious = kDir + "/previous";
const std::string kStaging = kDir + "/staging";
const std::string kTrash = kDir + "/trash";
const std::string kZip = kDir + "/download.zip";
const std::string kAvailable = kDir + "/available.json";
constexpr const char *kBootAttempts = ONION_USER_DATA_DIR "/boot_attempts";
constexpr const char *kAutostartDone = ONION_USER_DATA_DIR "/boot_autostart_done";

constexpr size_t kMaxZipBytes = 64u << 20;
constexpr mz_uint64 kMaxUnpackedBytes = 128u << 20;
constexpr mz_uint kMaxEntries = 32;
/* left free on /data on top of what an update needs */
constexpr unsigned long long kSpareBytes = 64ull << 20;
constexpr size_t kMaxNotes = 16000;

struct Job {
  std::string state = "idle"; /* idle check download unpack install done failed */
  std::string action, tag, err, msg;
  size_t got = 0, total = 0;
};

std::mutex g_mu;
Job g_job;
std::atomic_bool g_cancel{false};
/* state.json: the job, a rollback and the boot-OK thread all rewrite it */
std::mutex g_state_mu;

bool busy_locked() {
  return g_job.state == "check" || g_job.state == "download" || g_job.state == "unpack" ||
         g_job.state == "install" || g_job.state == "applying";
}

void set(const char *state, const std::string &err = "", const std::string &msg = "") {
  std::lock_guard<std::mutex> lock(g_mu);
  g_job.state = state;
  g_job.err = err;
  g_job.msg = msg;
}

void progress(size_t received, size_t total, void *) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_job.got = received;
  if (total) /* else keep the size GitHub gave */
    g_job.total = total;
}

bool cancelled(void *) { return g_cancel.load(); }

std::string hex(const uint8_t *d, size_t n) {
  static const char k[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += k[d[i] >> 4];
    s += k[d[i] & 15];
  }
  return s;
}

/* the release number of a tag or a build: "v0.1.0-rc16" / "0.1.0-rc16+ab12" -> "0.1.0-rc16" */
std::string bare(const std::string &v) {
  std::string s = v.size() && v[0] == 'v' ? v.substr(1) : v;
  return s.substr(0, s.find('+'));
}

bool tag_ok(const std::string &t) {
  if (t.empty() || t.size() > 40)
    return false;
  for (char c : t)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '+')
      return false;
  long key[4];
  return onion_update_version_key(t.c_str(), key);
}

bool is_pre(const std::string &v) {
  long key[4];
  return onion_update_version_key(v.c_str(), key) && key[3] != 1000000;
}

bool channel_all() {
  return g_settings.snapshot().update_channel == onion::kUpdateChannelPrerelease;
}

/* update.channel: prerelease takes everything; stable takes final releases,
 * and a pre-release only for whoever already runs one */
bool channel_takes(const std::string &tag, bool prerelease_flag) {
  return channel_all() || !(prerelease_flag || is_pre(tag)) || is_pre(ONIONHEN_VERSION);
}

/* a version, or a SHA-256 in hex */
std::string word(const std::string &path) {
  char out[96];
  onion_update_read_word(path.c_str(), out, sizeof(out));
  return out;
}

unsigned long long free_bytes(const char *path) {
  struct statvfs st {};
  if (statvfs(path, &st) != 0)
    return ~0ull;
  return static_cast<unsigned long long>(st.f_bavail) *
         static_cast<unsigned long long>(st.f_frsize);
}

std::string room_for(unsigned long long need) {
  const unsigned long long have = free_bytes(kDir.c_str());
  if (have == ~0ull || have >= need + kSpareBytes)
    return "";
  char b[128];
  snprintf(b, sizeof(b), "not enough free space: %.0f MB needed, %.0f MB free",
           static_cast<double>(need + kSpareBytes) / 1048576.0,
           static_cast<double>(have) / 1048576.0);
  return b;
}

/* rmtree() logs an error for a folder that is not there */
void drop(const std::string &path) {
  if (if_exists(path.c_str()))
    rmtree(path.c_str());
}

bool fetch_json(const std::string &url, std::string &body) {
  using namespace onion::cheats::sync;
  HttpRequest request;
  request.url = url.c_str();
  request.host_allow = "api.github.com";
  request.accept = "application/vnd.github+json";
  request.user_agent = ONION_BRAND_NAME;
  request.timeout_ms = 30000;
  request.max_body_bytes = 4u << 20;
  request.should_cancel = cancelled;
  body.clear();
  Ps5HttpTransport http;
  const SyncStatus st = http.perform(request, [&](const void *data, size_t n) {
    body.append(static_cast<const char *>(data), n);
    return SyncStatus::Ok;
  });
  if (st != SyncStatus::Ok)
    LOG_WARN("[neohen-update] %s: %s", url.c_str(), sync_status_name(st));
  return st == SyncStatus::Ok;
}

/* "sha256:<64 hex>" of a GitHub asset; "" when GitHub has none */
std::string asset_sha256(const cJSON *asset) {
  const std::string d = onion_cjson::string_item(asset, "digest", "");
  if (d.size() != 7 + 64 || d.compare(0, 7, "sha256:") != 0)
    return "";
  for (size_t i = 7; i < d.size(); ++i)
    if (!std::isdigit(static_cast<unsigned char>(d[i])) && (d[i] < 'a' || d[i] > 'f'))
      return "";
  return d.substr(7);
}

/* the release zip, as make_install_zip.sh names it */
const cJSON *release_zip(const cJSON *release, const std::string &tag) {
  const std::string want = "NeoHEN-" + bare(tag) + ".zip";
  const cJSON *assets = onion_cjson::item(release, "assets");
  for (const cJSON *a = assets ? assets->child : nullptr; a; a = a->next)
    if (want == onion_cjson::string_item(a, "name", ""))
      return a;
  return nullptr;
}

/* cut at a UTF-8 character boundary */
std::string clip(const std::string &s, size_t n) {
  if (s.size() <= n)
    return s;
  while (n && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
    --n;
  return s.substr(0, n) + "\n…";
}

/* what NeoWEB shows for one release */
cJSON *describe(const cJSON *r) {
  const std::string tag = onion_cjson::string_item(r, "tag_name", "");
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "tag", tag.c_str());
  cJSON_AddStringToObject(o, "name", onion_cjson::string_item(r, "name", ""));
  cJSON_AddStringToObject(o, "date", std::string(onion_cjson::string_item(r, "published_at", "")).substr(0, 10).c_str());
  cJSON_AddBoolToObject(o, "prerelease", onion_cjson::bool_item(r, "prerelease", false) || is_pre(tag));
  cJSON_AddStringToObject(o, "url", onion_cjson::string_item(r, "html_url", ""));
  cJSON_AddStringToObject(o, "notes", clip(onion_cjson::string_item(r, "body", ""), kMaxNotes).c_str());
  const cJSON *zip = release_zip(r, tag);
  const std::string sha = zip ? asset_sha256(zip) : "";
  const std::string url = zip ? onion_cjson::string_item(zip, "browser_download_url", "") : "";
  cJSON_AddStringToObject(o, "asset", zip ? onion_cjson::string_item(zip, "name", "") : "");
  cJSON_AddNumberToObject(o, "size", zip ? onion_cjson::int_item(zip, "size", 0) : 0);
  cJSON_AddStringToObject(o, "sha256", sha.c_str());
  const char *why = !zip ? "the release has no NeoHEN zip"
                  : sha.empty() ? "GitHub gives no SHA-256 for its zip"
                  : url.rfind(kDownloads, 0) != 0 ? "the zip is not in NeoHEN's releases"
                  : "";
  cJSON_AddBoolToObject(o, "installable", !*why);
  cJSON_AddStringToObject(o, "why", why);
  return o;
}

void write_json(const std::string &path, cJSON *root) {
  char *text = cJSON_PrintUnformatted(root);
  if (text) {
    mkdir_tree(kDir.c_str());
    write_file_atomic(path.c_str(), text, strlen(text));
    cJSON_free(text);
  }
}

void check_job() {
  std::string body;
  if (!fetch_json(std::string(kApi) + "/releases?per_page=20", body)) {
    set("failed", g_cancel.load() ? "cancelled" : "GitHub is not reachable");
    return;
  }
  onion_cjson::Root root(body);
  if (!root || !cJSON_IsArray(root.get())) {
    set("failed", "GitHub's answer is not a release list");
    return;
  }
  /* newest release of any kind and newest final one, as update_check.cpp */
  const cJSON *any = nullptr, *fin = nullptr;
  for (const cJSON *r = root.get()->child; r; r = r->next) {
    if (onion_cjson::bool_item(r, "draft", false))
      continue;
    const std::string tag = onion_cjson::string_item(r, "tag_name", "");
    if (!tag_ok(tag))
      continue;
    if (!any || onion_update_version_cmp(tag.c_str(), onion_cjson::string_item(any, "tag_name", "")) > 0)
      any = r;
    const bool pre = is_pre(tag) || onion_cjson::bool_item(r, "prerelease", false);
    if (!pre && (!fin || onion_update_version_cmp(tag.c_str(), onion_cjson::string_item(fin, "tag_name", "")) > 0))
      fin = r;
  }
  const char *mine = ONIONHEN_VERSION;
  auto newer = [&](const cJSON *r) {
    return r && onion_update_version_cmp(onion_cjson::string_item(r, "tag_name", ""), mine) > 0;
  };
  const bool all = channel_all();
  const cJSON *best = all ? any : newer(fin) ? fin : is_pre(mine) ? any : fin;
  cJSON *out = cJSON_CreateObject();
  cJSON_AddNumberToObject(out, "checked_at", static_cast<double>(time(nullptr)));
  cJSON_AddStringToObject(out, "channel", all ? "prerelease" : "stable");
  cJSON_AddStringToObject(out, "running", mine);
  cJSON_AddBoolToObject(out, "newer", newer(best));
  if (best)
    cJSON_AddItemToObject(out, "release", describe(best));
  else
    cJSON_AddNullToObject(out, "release");
  write_json(kAvailable, out);
  cJSON_Delete(out);
  const std::string tag = best ? onion_cjson::string_item(best, "tag_name", "") : "";
  LOG_INFO("[neohen-update] checked (%s): %s%s", all ? "prerelease" : "stable",
           tag.empty() ? "no release" : tag.c_str(), newer(best) ? " is newer" : "");
  set("done", "", newer(best) ? "new: " + tag : "up to date");
}

/* zip -> kStaging; one NeoHEN-<ver>/ folder of plain files */
std::string unpack(const std::string &ver) {
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  if (!mz_zip_reader_init_file(&zip, kZip.c_str(), 0))
    return "not a zip file";
  const std::string prefix = "NeoHEN-" + ver + "/";
  const mz_uint n = mz_zip_reader_get_num_files(&zip);
  std::string err;
  mz_uint64 total = 0;
  bool elf = false, leeme = false, terceros = false;
  if (n == 0 || n > kMaxEntries)
    err = "not a NeoHEN release zip (no or too many files)";
  for (mz_uint i = 0; err.empty() && i < n; ++i) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&zip, i, &st)) {
      err = "unreadable zip entry";
      break;
    }
    const std::string name(st.m_filename);
    if (name.compare(0, prefix.size(), prefix) != 0) {
      err = "the zip is not one " + prefix + " folder";
      break;
    }
    const std::string rel = name.substr(prefix.size());
    if (rel.empty())
      continue;
    bool plain = rel.size() < 64 && rel[0] != '.';
    for (char c : rel)
      plain = plain && (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-');
    if (!plain || mz_zip_reader_is_file_a_directory(&zip, i)) {
      err = "unexpected entry in the zip: " + rel;
      break;
    }
    elf = elf || rel == ONION_UPDATE_ELF;
    leeme = leeme || rel == "LEEME.txt";
    terceros = terceros || rel == "TERCEROS.txt";
    total += st.m_uncomp_size;
  }
  if (err.empty() && (!elf || !leeme || !terceros))
    err = "the zip lacks NeoHEN.elf, LEEME.txt or TERCEROS.txt";
  if (err.empty() && total > kMaxUnpackedBytes)
    err = "the release is too big";
  if (err.empty())
    err = room_for(static_cast<unsigned long long>(total));
  if (err.empty()) {
    drop(kStaging);
    if (!mkdir_tree(kStaging.c_str()))
      err = "cannot create the staging folder";
  }
  for (mz_uint i = 0; err.empty() && i < n; ++i) {
    if (g_cancel.load()) {
      err = "cancelled";
      break;
    }
    mz_zip_archive_file_stat st;
    mz_zip_reader_file_stat(&zip, i, &st);
    const std::string rel = std::string(st.m_filename).substr(prefix.size());
    if (rel.empty())
      continue;
    const std::string dest = kStaging + "/" + rel;
    if (!mz_zip_reader_extract_to_file(&zip, i, dest.c_str(), 0)) {
      err = "cannot unpack " + rel + " (is the drive full?)";
      break;
    }
    chmod(dest.c_str(), 0777);
  }
  mz_zip_reader_end(&zip);
  return err;
}

/* staging/NeoHEN.elf is an x86-64 ELF and the release <ver>; *sha its hash */
std::string check_elf(const std::string &ver, std::string *sha) {
  uint8_t *buf = nullptr;
  size_t size = 0;
  const std::string path = kStaging + "/" ONION_UPDATE_ELF;
  if (!read_file_alloc(path.c_str(), kMaxUnpackedBytes, &buf, &size))
    return "NeoHEN.elf cannot be read";
  std::string err;
  if (size < 64 || std::memcmp(buf, "\x7f" "ELF", 4) != 0 || buf[4] != 2 /* 64-bit */ ||
      buf[5] != 1 /* little endian */ || (buf[18] | (buf[19] << 8)) != 62 /* x86-64 */) {
    err = "NeoHEN.elf is not a PS5 ELF";
  } else {
    const char *marker = ONION_UPDATE_MARKER;
    const uint8_t *end = buf + size;
    const uint8_t *at = std::search(static_cast<const uint8_t *>(buf), end, marker, marker + strlen(marker));
    std::string inside;
    if (at != end)
      for (const uint8_t *p = at + strlen(marker);
           p < end && inside.size() < ONION_UPDATE_VER_MAX && *p &&
           (std::isalnum(*p) || *p == '.' || *p == '-' || *p == '+');
           ++p)
        inside += static_cast<char>(*p);
    if (inside.empty())
      err = "NeoHEN.elf does not say which release it is (releases before 0.1.0-rc16 cannot be installed this way)";
    else if (bare(inside) != ver)
      err = "NeoHEN.elf is " + bare(inside) + ", not " + ver;
  }
  if (err.empty()) {
    SHA256_CTX ctx;
    uint8_t d[SHA256_BLOCK_SIZE];
    sha256_init(&ctx);
    sha256_update(&ctx, buf, size);
    sha256_final(&ctx, d);
    *sha = hex(d, sizeof(d));
  }
  free(buf);
  return err;
}

bool write_word(const std::string &path, const std::string &text) {
  const std::string line = text + "\n";
  return write_file_atomic(path.c_str(), line.data(), line.size());
}

/* staging -> current, current -> previous, the old previous goes */
std::string swap_in(const std::string &ver) {
  std::lock_guard<std::mutex> lock(g_state_mu);
  onion_update_state st;
  onion_update_state_load(kDir.c_str(), &st);
  const std::string old_cur = word(kCurrent + "/" ONION_UPDATE_VERSION_FILE);
  drop(kTrash);
  const bool had_prev = if_exists(kPrevious.c_str());
  const bool had_cur = if_exists(kCurrent.c_str());
  if (had_prev && rename(kPrevious.c_str(), kTrash.c_str()) != 0)
    return "cannot move the older release aside";
  if (had_cur && rename(kCurrent.c_str(), kPrevious.c_str()) != 0) {
    if (had_prev)
      rename(kTrash.c_str(), kPrevious.c_str());
    return "cannot move the installed release aside";
  }
  if (rename(kStaging.c_str(), kCurrent.c_str()) != 0) {
    if (had_cur)
      rename(kPrevious.c_str(), kCurrent.c_str());
    if (had_prev)
      rename(kTrash.c_str(), kPrevious.c_str());
    return "cannot put the new release in place";
  }
  drop(kTrash);
  onion_update_state out;
  memset(&out, 0, sizeof(out));
  snprintf(out.status, sizeof(out.status), "trial");
  onion_update_clean(out.current, sizeof(out.current), ver.c_str());
  onion_update_clean(out.previous, sizeof(out.previous), had_cur ? old_cur.c_str() : "");
  onion_update_clean(out.from, sizeof(out.from), ONIONHEN_VERSION);
  out.installed_at = static_cast<long>(time(nullptr));
  if (!onion_update_state_save(kDir.c_str(), &out))
    LOG_WARN("[neohen-update] state.json could not be written (the release is in place)");
  return "";
}

void install_job(const std::string &tag) {
  const std::string ver = bare(tag);
  mkdir_tree(kDir.c_str()); /* statvfs needs it to be there */
  std::string body;
  if (!fetch_json(std::string(kApi) + "/releases/tags/" + tag, body)) {
    set("failed", g_cancel.load() ? "cancelled" : "GitHub is not reachable, or there is no release " + tag);
    return;
  }
  onion_cjson::Root rel(body);
  if (!rel || !cJSON_IsObject(rel.get()) || onion_cjson::bool_item(rel.get(), "draft", false) ||
      std::string(onion_cjson::string_item(rel.get(), "tag_name", "")) != tag) {
    set("failed", "no release " + tag);
    return;
  }
  if (!channel_takes(tag, onion_cjson::bool_item(rel.get(), "prerelease", false))) {
    set("failed", tag + " is a pre-release and update.channel is stable");
    return;
  }
  const cJSON *zip = release_zip(rel.get(), tag);
  const std::string sha = zip ? asset_sha256(zip) : "";
  const std::string url = zip ? onion_cjson::string_item(zip, "browser_download_url", "") : "";
  const double size = zip ? onion_cjson::int_item(zip, "size", 0) : 0;
  if (!zip || url.rfind(kDownloads, 0) != 0) {
    set("failed", "release " + tag + " has no NeoHEN-" + ver + ".zip");
    return;
  }
  /* decision 6.1 of the 1.0 plan: nothing without a hash */
  if (sha.empty()) {
    set("failed", "GitHub gives no SHA-256 for NeoHEN-" + ver + ".zip: not installed");
    return;
  }
  if (size <= 0 || size > kMaxZipBytes) {
    set("failed", "the release zip has an unexpected size");
    return;
  }
  /* the zip, what it unpacks to, and the copy already in current/ */
  std::string err = room_for(static_cast<unsigned long long>(size) * 3);
  if (!err.empty()) {
    set("failed", err);
    return;
  }

  using namespace onion::cheats::sync;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_job.state = "download";
    g_job.got = 0;
    g_job.total = static_cast<size_t>(size);
  }
  FILE *out = fopen(kZip.c_str(), "wb");
  SHA256_CTX ctx;
  sha256_init(&ctx);
  SyncStatus st = out ? SyncStatus::Ok : SyncStatus::Io;
  if (out) {
    HttpRequest request;
    request.url = url.c_str();
    request.host_allow = "github.com";
    request.accept = "application/octet-stream";
    request.user_agent = ONION_BRAND_NAME;
    request.timeout_ms = 300000;
    request.max_body_bytes = kMaxZipBytes;
    request.on_progress = progress;
    request.should_cancel = cancelled;
    Ps5HttpTransport http;
    st = http.perform(request, [&](const void *data, size_t n) {
      sha256_update(&ctx, static_cast<const uint8_t *>(data), n);
      return fwrite(data, 1, n, out) == n ? SyncStatus::Ok : SyncStatus::Io;
    });
    if (fclose(out) != 0 && st == SyncStatus::Ok)
      st = SyncStatus::Io;
  }
  if (st != SyncStatus::Ok) {
    unlink(kZip.c_str());
    set("failed", g_cancel.load() ? "cancelled" : std::string("download: ") + sync_status_name(st));
    return;
  }
  uint8_t digest[SHA256_BLOCK_SIZE];
  sha256_final(&ctx, digest);
  if (hex(digest, sizeof(digest)) != sha) {
    unlink(kZip.c_str());
    LOG_ERROR("[neohen-update] %s: the download does not match GitHub's SHA-256", tag.c_str());
    set("failed", "the download does not match GitHub's SHA-256: thrown away");
    return;
  }

  set("unpack");
  err = unpack(ver);
  unlink(kZip.c_str());
  std::string elf_sha;
  if (err.empty())
    err = check_elf(ver, &elf_sha);
  if (err.empty() && (!write_word(kStaging + "/" ONION_UPDATE_VERSION_FILE, ver) ||
                      !write_word(kStaging + "/" ONION_UPDATE_SHA_FILE, elf_sha)))
    err = "cannot write to the staging folder";
  if (err.empty() && g_cancel.load())
    err = "cancelled";
  if (err.empty()) {
    set("install");
    err = swap_in(ver);
  }
  drop(kStaging);
  if (!err.empty()) {
    LOG_ERROR("[neohen-update] %s: %s", tag.c_str(), err.c_str());
    set("failed", err);
    return;
  }
  LOG_INFO("[neohen-update] %s installed in %s (was running %s)", ver.c_str(), kCurrent.c_str(),
           ONIONHEN_VERSION);
  onion_notify(true, "notify.update.installed_fmt", ver.c_str());
  set("done", "", "installed " + ver);
}

void *job_thread(void *) {
  Job j;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    j = g_job;
  }
  if (j.action == "check")
    check_job();
  else
    install_job(j.tag);
  return nullptr;
}

std::string status_json() {
  std::lock_guard<std::mutex> lock(g_mu);
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "state", g_job.state.c_str());
  cJSON_AddStringToObject(o, "action", g_job.action.c_str());
  cJSON_AddStringToObject(o, "tag", g_job.tag.c_str());
  cJSON_AddNumberToObject(o, "got", static_cast<double>(g_job.got));
  cJSON_AddNumberToObject(o, "total", static_cast<double>(g_job.total));
  cJSON_AddStringToObject(o, "err", g_job.err.substr(0, 200).c_str());
  cJSON_AddStringToObject(o, "msg", g_job.msg.substr(0, 200).c_str());
  cJSON_AddStringToObject(o, "running", ONIONHEN_VERSION);
  char *text = cJSON_PrintUnformatted(o);
  std::string s = text ? text : "{}";
  cJSON_free(text);
  cJSON_Delete(o);
  return s;
}

bool start_job(const std::string &action, const std::string &tag, std::string &out) {
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (busy_locked()) {
      out = "an update job is already running";
      return false;
    }
    g_job = Job();
    g_job.state = action == "check" ? "check" : "download";
    g_job.action = action;
    g_job.tag = tag;
  }
  g_cancel.store(false);
  pthread_t thread{};
  if (pthread_create(&thread, nullptr, job_thread, nullptr) != 0) {
    set("failed", "cannot start");
    out = "cannot start";
    return false;
  }
  pthread_detach(thread);
  out = status_json();
  return true;
}

bool rollback(std::string &out) {
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (busy_locked()) {
      out = "an update job is running";
      return false;
    }
  }
  std::lock_guard<std::mutex> lock(g_state_mu);
  if (!if_exists(kCurrent.c_str())) {
    out = "no NeoHEN installed from NeoWEB to go back from";
    return false;
  }
  onion_update_state st;
  onion_update_state_load(kDir.c_str(), &st);
  if (!onion_update_roll_back(kDir.c_str(), &st, "manual", static_cast<long>(time(nullptr)))) {
    out = "the files could not be moved";
    return false;
  }
  LOG_INFO("[neohen-update] rolled back by hand from %s to %s", st.rolled_back_from,
           st.current[0] ? st.current : "the NeoHEN.elf that is sent");
  {
    std::lock_guard<std::mutex> job_lock(g_mu);
    g_job = Job();
    g_job.action = "rollback";
  }
  set("done", "", std::string("back to ") + (st.current[0] ? st.current : "the NeoHEN.elf you send"));
  out = status_json();
  return true;
}

void *apply_thread(void *) {
  sleep(2); /* the answer to NeoWEB goes out first: this util is replaced too */
  uint8_t *elf = nullptr;
  size_t size = 0;
  const std::string path = kCurrent + "/" ONION_UPDATE_ELF;
  if (!read_file_alloc(path.c_str(), kMaxZipBytes, &elf, &size)) {
    set("failed", "the installed NeoHEN.elf cannot be read");
    return nullptr;
  }
  const bool sent = elfldr_remote_send_bytes_to(ELFLDR_REMOTE_PORT, elf, size);
  free(elf);
  LOG_INFO("[neohen-update] installed NeoHEN sent to %d: %s", ELFLDR_REMOTE_PORT,
           sent ? "ok" : "no answer");
  if (sent)
    set("done", "", "sent: NeoHEN is restarting with the installed release");
  else
    set("failed", "the ELF loader on port 9021 is not answering: start NeoHEN again by hand");
  return nullptr;
}

bool apply(std::string &out) {
  const std::string installed = word(kCurrent + "/" ONION_UPDATE_VERSION_FILE);
  const int cmp = onion_update_version_cmp(installed.c_str(), ONIONHEN_VERSION);
  if (installed.empty()) {
    out = "no NeoHEN installed from NeoWEB";
    return false;
  }
  if (cmp == 0) {
    out = installed + " is the one running";
    return false;
  }
  if (cmp < 0 && cmp != -2) {
    out = "the installed " + installed + " is older than the running NeoHEN";
    return false;
  }
  /* the same check the hand-off does */
  uint8_t *elf = nullptr;
  size_t size = 0;
  std::string got;
  if (read_file_alloc((kCurrent + "/" ONION_UPDATE_ELF).c_str(), kMaxZipBytes, &elf, &size)) {
    SHA256_CTX ctx;
    uint8_t d[SHA256_BLOCK_SIZE];
    sha256_init(&ctx);
    sha256_update(&ctx, elf, size);
    sha256_final(&ctx, d);
    got = hex(d, sizeof(d));
    free(elf);
  }
  if (got.empty() || got != word(kCurrent + "/" ONION_UPDATE_SHA_FILE)) {
    out = "the installed NeoHEN.elf does not match its SHA-256";
    return false;
  }
  if (sceSystemServiceGetAppIdOfRunningBigApp() > 0) {
    out = "close the open game first";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (busy_locked()) {
      out = "an update job is running";
      return false;
    }
    g_job = Job();
    g_job.state = "applying";
    g_job.action = "apply";
    g_job.tag = installed;
  }
  pthread_t thread{};
  if (pthread_create(&thread, nullptr, apply_thread, nullptr) != 0) {
    set("failed", "cannot start");
    out = "cannot start";
    return false;
  }
  pthread_detach(thread);
  LOG_INFO("[neohen-update] applying %s now (running %s)", installed.c_str(), ONIONHEN_VERSION);
  out = status_json();
  return true;
}

/* boot OK of the release on trial: the bootstrapper counted this start
 * (boot_attempts >= 1 while util starts) and the daemon puts the count
 * back to 0, by removing the file, 60 s after auto-start is over */
void *confirm_thread(void *) {
  for (int waited = 0; waited < 30 * 60; waited += 10) {
    sleep(10);
    std::lock_guard<std::mutex> lock(g_state_mu);
    onion_update_state st;
    onion_update_state_load(kDir.c_str(), &st);
    const std::string installed = word(kCurrent + "/" ONION_UPDATE_VERSION_FILE);
    if (strcmp(st.status, "trial") != 0 ||
        onion_update_version_cmp(installed.c_str(), ONIONHEN_VERSION) != 0 ||
        onion_update_version_cmp(st.current, installed.c_str()) != 0)
      return nullptr; /* nothing on trial, or not this NeoHEN */
    if (!if_exists(kAutostartDone) || if_exists(kBootAttempts))
      continue;
    snprintf(st.status, sizeof(st.status), "ok");
    st.confirmed_at = static_cast<long>(time(nullptr));
    onion_update_state_save(kDir.c_str(), &st);
    LOG_INFO("[neohen-update] %s reached boot OK: kept", installed.c_str());
    return nullptr;
  }
  return nullptr;
}

} // namespace

bool util_neohen_update(const cJSON *request, std::string &out) {
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
  if (action == "check")
    return start_job("check", "", out);
  if (action == "install") {
    const std::string tag = onion_cjson::string_item(request, "tag", "");
    if (!tag_ok(tag)) {
      out = "rejected";
      return false;
    }
    if (onion_update_version_cmp(tag.c_str(), ONIONHEN_VERSION) <= 0 &&
        onion_update_version_cmp(tag.c_str(), ONIONHEN_VERSION) != -2) {
      out = tag + " is not newer than the running NeoHEN";
      return false;
    }
    if (onion_update_version_cmp(tag.c_str(), word(kCurrent + "/" ONION_UPDATE_VERSION_FILE).c_str()) == 0) {
      out = tag + " is already installed (restart NeoHEN to use it)";
      return false;
    }
    LOG_INFO("[neohen-update] install %s (running %s)", tag.c_str(), ONIONHEN_VERSION);
    return start_job("install", tag, out);
  }
  if (action == "rollback")
    return rollback(out);
  if (action == "apply")
    return apply(out);
  out = "unknown action";
  return false;
}

void util_neohen_update_start() {
  pthread_t thread{};
  if (pthread_create(&thread, nullptr, confirm_thread, nullptr) == 0)
    pthread_detach(thread);
  else
    LOG_ERROR("[neohen-update] cannot start the boot OK thread");
}
