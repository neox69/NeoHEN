/* NeoHEN: says on the TV when a newer NeoHEN is out.
 *
 * 90 s after start and then every 12 hours the util service asks GitHub for
 * the releases of neox69/NeoHEN (pre-releases too) and compares the newest
 * with the NeoHEN that is running. Each new version is announced once: the
 * tag shown last is kept in update_seen.txt. An "update_check_off" file in
 * the data folder (NeoWEB > Settings switches it) turns it off. Nothing is
 * downloaded: the notice only says which version there is. */
#include "cheats/sync/http_transport_ps5.hpp"
#include "cheats/sync/types.hpp"
#include "onion_cjson.hpp"
#include <onion/brand.h>
#include <onion/log.h>
#include <onion/notify.h>
#include <onion/version.h>

#include <cstdio>
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

void *update_thread(void *) {
  sleep(kFirstCheckS);
  for (;;) {
    check_once();
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
