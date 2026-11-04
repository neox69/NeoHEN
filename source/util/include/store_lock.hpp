/* NeoHEN: the lock that util and NeoWEB share for web/store.json.
 *
 * Both rewrite store.json (NeoWEB when the store installs a payload, util
 * when it updates one by itself). Each side takes store.json.lock with
 * O_CREAT|O_EXCL, re-reads the file, changes its part, writes a temp file
 * of its own and renames it over, then removes the lock. A lock older than
 * kStaleS seconds is from a writer that died and is taken over; after
 * kWaitMs without it the write goes ahead anyway (it is still atomic). The
 * other side of this is store_lock() in web/src/api_store.c. */
#pragma once

#include <onion/brand.h>

#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

class util_store_lock {
public:
  static constexpr const char *kPath = ONION_USER_DATA_DIR "/web/store.json.lock";
  static constexpr int kStaleS = 30;
  static constexpr int kWaitMs = 5000;

  util_store_lock() {
    for (int waited = 0; waited <= kWaitMs; waited += 100) {
      const int fd = open(kPath, O_WRONLY | O_CREAT | O_EXCL, 0666);
      if (fd >= 0) {
        close(fd);
        held_ = true;
        return;
      }
      if (errno != EEXIST)
        return; /* no web folder yet, or read-only: nothing to guard */
      struct stat st {};
      if (stat(kPath, &st) == 0 && time(nullptr) - st.st_mtime > kStaleS) {
        unlink(kPath);
        continue;
      }
      usleep(100 * 1000);
    }
  }
  ~util_store_lock() {
    if (held_)
      unlink(kPath);
  }
  util_store_lock(const util_store_lock &) = delete;
  util_store_lock &operator=(const util_store_lock &) = delete;

private:
  bool held_ = false;
};
