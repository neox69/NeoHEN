/* NeoHEN: says on the TV when a newer NeoHEN is out.
 *
 * 90 s after start and then every 12 hours the util service asks GitHub for
 * the releases of neox69/NeoHEN (pre-releases too) and compares the newest
 * with the NeoHEN that is running. Each new version is announced once: the
 * tag shown last is kept in update_seen.txt. An "update_check_off" file in
 * the data folder (NeoWEB > Settings switches it) turns it off. Nothing is
 * downloaded for NeoHEN itself: the notice only says which version there is.
 * The same thread keeps the payloads installed from NeoWEB's store up to date. */
#include "cheats/sync/http_transport_ps5.hpp"
#include "cheats/sync/types.hpp"
#include "onion_cjson.hpp"
#include <onion/brand.h>
#include <onion/log.h>
#include <onion/notify.h>
#include <onion/version.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>
#include <cstdlib>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr const char *kReleases =
    "https://api.github.com/repos/neox69/NeoHEN/releases?per_page=10";
constexpr const char *kSeen = ONION_USER_DATA_DIR "/update_seen.txt";
constexpr const char *kOff = ONION_USER_DATA_DIR "/update_check_off";
constexpr unsigned kFirstCheckS = 90;
constexpr unsigned kEveryS = 12 * 60 * 60;

/* 0.1.0-rc7 -> {0,1,0,7}; a final release ranks above its rcs. */
bool version_key(const std::string &text, long out[4]) {
  const char *s = text.c_str();
  if (*s == 'v')
    ++s;
  char *end = nullptr;
  for (int i = 0; i < 3; ++i) {
    out[i] = std::strtol(s, &end, 10);
    if (end == s)
      return false;
    s = end;
    if (i < 2) {
      if (*s != '.')
        return false;
      ++s;
    }
  }
  out[3] = 1000000;
  if (std::string(s).rfind("-rc", 0) == 0)
    out[3] = std::strtol(s + 3, nullptr, 10);
  return true;
}

int compare(const long a[4], const long b[4]) {
  for (int i = 0; i < 4; ++i)
    if (a[i] != b[i])
      return a[i] < b[i] ? -1 : 1;
  return 0;
}

std::string read_line(const char *path) {
  std::string out;
  if (FILE *f = fopen(path, "rb")) {
    char line[64] = {};
    if (fgets(line, sizeof(line), f))
      out = line;
    fclose(f);
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
    out.pop_back();
  return out;
}

void check_once() {
  struct stat st {};
  if (stat(kOff, &st) == 0)
    return;
  long mine[4];
  if (!version_key(ONIONHEN_VERSION, mine))
    return;
  using namespace onion::cheats::sync;
  HttpRequest request;
  request.url = kReleases;
  request.host_allow = "api.github.com";
  request.accept = "application/vnd.github+json";
  request.user_agent = ONION_BRAND_NAME;
  request.timeout_ms = 30000;
  request.max_body_bytes = 2u << 20;
  std::string body;
  Ps5HttpTransport http;
  const SyncStatus result = http.perform(request, [&](const void *data, size_t n) {
    body.append(static_cast<const char *>(data), n);
    return SyncStatus::Ok;
  });
  if (result != SyncStatus::Ok) {
    LOG_INFO("[update] GitHub not reachable (%s)", sync_status_name(result));
    return;
  }
  onion_cjson::Root root(body);
  if (!root || !cJSON_IsArray(root.get()))
    return;
  std::string best_tag;
  long best[4] = {0, 0, 0, 0};
  for (const cJSON *r = root.get()->child; r; r = r->next) {
    if (onion_cjson::bool_item(r, "draft", false))
      continue;
    const std::string tag = onion_cjson::string_item(r, "tag_name", "");
    long key[4];
    if (version_key(tag, key) && (best_tag.empty() || compare(key, best) > 0)) {
      best_tag = tag;
      for (int i = 0; i < 4; ++i)
        best[i] = key[i];
    }
  }
  if (best_tag.empty() || compare(best, mine) <= 0) {
    LOG_INFO("[update] up to date (%s)", ONIONHEN_VERSION);
    return;
  }
  if (read_line(kSeen) == best_tag)
    return; /* already said */
  LOG_INFO("[update] %s is out (running %s)", best_tag.c_str(), ONIONHEN_VERSION);
  onion_notify(true, "notify.update.available_fmt", best_tag.c_str() + (best_tag[0] == 'v'));
  if (FILE *f = fopen(kSeen, "wb")) {
    fprintf(f, "%s\n", best_tag.c_str());
    fclose(f);
  }
}

/* ------------------------------------------------ payloads from the store
 * NeoWEB's store keeps, for each payload it installed, the release it put
 * ("installed") and the file it came from ("sources", a github.com release
 * download URL). When the author has a newer release with a file of the same
 * name, it replaces the one in the payloads folder. A payload that is running
 * keeps running the old version until it is started again. Payloads put
 * there by hand or bundled with NeoHEN are not in the store: never touched.
 * "store_auto_off" in the data folder turns this off. */
constexpr const char *kStore = ONION_USER_DATA_DIR "/web/store.json";
constexpr const char *kStoreOff = ONION_USER_DATA_DIR "/store_auto_off";

std::string read_file(const char *path) {
  std::string out;
  if (FILE *f = fopen(path, "rb")) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < (1u << 20))
      out.append(buf, n);
    fclose(f);
  }
  return out;
}

bool fetch_text(const std::string &url, const char *host, std::string &body) {
  using namespace onion::cheats::sync;
  HttpRequest request;
  request.url = url.c_str();
  request.host_allow = host;
  request.accept = "application/vnd.github+json";
  request.user_agent = ONION_BRAND_NAME;
  request.timeout_ms = 30000;
  request.max_body_bytes = 2u << 20;
  body.clear();
  Ps5HttpTransport http;
  return http.perform(request, [&](const void *data, size_t n) {
           body.append(static_cast<const char *>(data), n);
           return SyncStatus::Ok;
         }) == SyncStatus::Ok;
}

/* Same rules as the store's own download: github.com only, an ELF for .elf. */
bool download(const std::string &url, const std::string &name) {
  const std::string dest = ONION_USER_DATA_DIR "/payloads/" + name;
  const std::string part = dest + ".part";
  FILE *out = fopen(part.c_str(), "wb");
  if (!out)
    return false;
  using namespace onion::cheats::sync;
  HttpRequest request;
  request.url = url.c_str();
  request.host_allow = "github.com";
  request.accept = "application/octet-stream";
  request.timeout_ms = 120000;
  request.max_body_bytes = 64u << 20;
  size_t bytes = 0;
  unsigned char head[4] = {};
  Ps5HttpTransport http;
  const SyncStatus st = http.perform(request, [&](const void *data, size_t n) {
    if (bytes < sizeof(head))
      std::memcpy(head + bytes, data, std::min(n, sizeof(head) - bytes));
    bytes += n;
    return fwrite(data, 1, n, out) == n ? SyncStatus::Ok : SyncStatus::Io;
  });
  const bool closed = fclose(out) == 0;
  const bool is_elf = name.size() > 4 && name.compare(name.size() - 4, 4, ".elf") == 0;
  const bool looks_right = !is_elf || (bytes > 4 && std::memcmp(head, "\x7f" "ELF", 4) == 0);
  if (st != SyncStatus::Ok || !closed || bytes == 0 || !looks_right ||
      rename(part.c_str(), dest.c_str()) != 0) {
    unlink(part.c_str());
    LOG_WARN("[store] update of %s failed (%s, %zu bytes)", name.c_str(),
             sync_status_name(st), bytes);
    return false;
  }
  chmod(dest.c_str(), 0777);
  return true;
}

void store_check_once() {
  struct stat st {};
  if (stat(kStoreOff, &st) == 0)
    return;
  onion_cjson::Root store(read_file(kStore));
  if (!store)
    return;
  const cJSON *sources = onion_cjson::item(store.get(), "sources");
  const cJSON *installed = onion_cjson::item(store.get(), "installed");
  if (!sources || !installed)
    return;
  struct Update { std::string name, from, to, url; };
  std::vector<Update> todo;
  for (const cJSON *src = sources->child; src; src = src->next) {
    const std::string name = src->string ? src->string : "";
    const std::string url = onion_cjson::string_value(src, "");
    /* https://github.com/OWNER/REPO/releases/download/TAG/ASSET */
    const std::string pre = "https://github.com/";
    if (name.empty() || url.rfind(pre, 0) != 0)
      continue;
    std::vector<std::string> parts;
    size_t from = pre.size();
    for (size_t at; (at = url.find('/', from)) != std::string::npos; from = at + 1)
      parts.push_back(url.substr(from, at - from));
    parts.push_back(url.substr(from));
    if (parts.size() != 6 || parts[2] != "releases" || parts[3] != "download")
      continue;
    const std::string asset = parts[5];
    const std::string current = onion_cjson::string_item(installed, name.c_str(), "");
    std::string body;
    if (!fetch_text("https://api.github.com/repos/" + parts[0] + "/" + parts[1] +
                        "/releases?per_page=1",
                    "api.github.com", body))
      continue;
    onion_cjson::Root rel(body);
    const cJSON *latest = rel && cJSON_IsArray(rel.get()) ? rel.get()->child : nullptr;
    if (!latest || onion_cjson::bool_item(latest, "draft", false))
      continue;
    const std::string tag = onion_cjson::string_item(latest, "tag_name", "");
    if (tag.empty() || tag == current)
      continue;
    const cJSON *assets = onion_cjson::item(latest, "assets");
    for (const cJSON *a = assets ? assets->child : nullptr; a; a = a->next) {
      if (asset == onion_cjson::string_item(a, "name", "")) {
        todo.push_back({name, current, tag,
                        onion_cjson::string_item(a, "browser_download_url", "")});
        break;
      }
    }
  }
  if (todo.empty())
    return;
  bool changed = false;
  cJSON *root = store.get();
  cJSON *inst = cJSON_GetObjectItemCaseSensitive(root, "installed");
  cJSON *srcs = cJSON_GetObjectItemCaseSensitive(root, "sources");
  cJSON *log = cJSON_GetObjectItemCaseSensitive(root, "updates");
  if (!cJSON_IsArray(log)) {
    cJSON_DeleteItemFromObjectCaseSensitive(root, "updates");
    log = cJSON_AddArrayToObject(root, "updates");
  }
  for (const Update &u : todo) {
    if (u.url.rfind("https://github.com/", 0) != 0 || !download(u.url, u.name))
      continue;
    LOG_INFO("[store] %s updated %s -> %s", u.name.c_str(), u.from.c_str(), u.to.c_str());
    onion_notify(true, "notify.store.updated_fmt", u.name.c_str(), u.to.c_str());
    cJSON_ReplaceItemInObjectCaseSensitive(inst, u.name.c_str(), cJSON_CreateString(u.to.c_str()));
    cJSON_ReplaceItemInObjectCaseSensitive(srcs, u.name.c_str(), cJSON_CreateString(u.url.c_str()));
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "name", u.name.c_str());
    cJSON_AddStringToObject(e, "from", u.from.c_str());
    cJSON_AddStringToObject(e, "to", u.to.c_str());
    cJSON_AddNumberToObject(e, "time", static_cast<double>(time(nullptr)));
    cJSON_InsertItemInArray(log, 0, e);
    while (cJSON_GetArraySize(log) > 20)
      cJSON_DeleteItemFromArray(log, 20);
    changed = true;
  }
  if (!changed)
    return;
  char *text = cJSON_PrintUnformatted(root);
  if (!text)
    return;
  const std::string tmp = std::string(kStore) + ".tmp";
  if (FILE *f = fopen(tmp.c_str(), "wb")) {
    fputs(text, f);
    fclose(f);
    rename(tmp.c_str(), kStore);
  }
  cJSON_free(text);
}

void *update_thread(void *) {
  sleep(kFirstCheckS);
  for (;;) {
    check_once();
    store_check_once();
    sleep(kEveryS);
  }
  return nullptr;
}

} // namespace

void util_update_check_start() {
  pthread_t thread;
  if (pthread_create(&thread, nullptr, update_thread, nullptr) == 0)
    pthread_detach(thread);
  else
    LOG_ERROR("[update] cannot start the check thread");
}
