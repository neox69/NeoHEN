/* Copyright (C) 2026 OnionHEN / LightningMods */

#include <onion/brand.h>
#include "payload_autostart.h"

#include "bootstrap_notify.h"
#include "launch_pipeline.h"

#include <elfldr_remote.h>
#include <onion/log.h>
#include <onion/payload.h>
#include <onion/payload_config.h>
#include <onion/platform.h>

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct PayloadEntry {
  char *path;
  char *filename;
  OnionPayloadConfig config;
} PayloadEntry;

typedef struct PayloadList {
  PayloadEntry *entries;
  size_t count;
} PayloadList;

static const char *const kPayloadDirectories[] = {
    "/mnt/usb0/" ONION_DIR_NAME_LOWER "/payloads", "/mnt/usb0/" ONION_DIR_NAME "/payloads",
    "/mnt/usb1/" ONION_DIR_NAME_LOWER "/payloads", "/mnt/usb1/" ONION_DIR_NAME "/payloads",
    "/mnt/usb2/" ONION_DIR_NAME_LOWER "/payloads", "/mnt/usb2/" ONION_DIR_NAME "/payloads",
    "/mnt/usb3/" ONION_DIR_NAME_LOWER "/payloads", "/mnt/usb3/" ONION_DIR_NAME "/payloads",
    ONION_USER_DATA_DIR "/payloads",
    "/user/data/" ONION_DIR_NAME_LOWER "/payloads", ONION_DATA_DIR "/payloads",
    "/data/" ONION_DIR_NAME_LOWER "/payloads",
};

static void payload_list_destroy(PayloadList *list) {
  if (!list)
    return;
  for (size_t i = 0; i < list->count; ++i) {
    free(list->entries[i].path);
    free(list->entries[i].filename);
  }
  free(list->entries);
  list->entries = NULL;
  list->count = 0;
}

static bool payload_list_contains_path(const PayloadList *list,
                                       const char *path) {
  for (size_t i = 0; i < list->count; ++i) {
    if (strcmp(list->entries[i].path, path) == 0)
      return true;
  }
  return false;
}

static bool payload_list_append(PayloadList *list, const char *path,
                                const char *filename) {
  PayloadEntry *entries = (PayloadEntry *)realloc(
      list->entries, (list->count + 1) * sizeof(*list->entries));
  if (!entries)
    return false;

  list->entries = entries;
  PayloadEntry *entry = &list->entries[list->count];
  entry->path = strdup(path);
  entry->filename = strdup(filename);
  if (!onion_payload_config_load(path, &entry->config))
    LOG_WARN("Invalid payload config, using defaults: %s", path);
  if (!entry->path || !entry->filename) {
    free(entry->path);
    free(entry->filename);
    entry->path = NULL;
    entry->filename = NULL;
    return false;
  }
  ++list->count;
  return true;
}

static bool is_elf_filename(const char *filename) {
  const char *extension = strrchr(filename, '.');
  return extension && strcmp(extension, ".elf") == 0;
}

static void scan_payload_directory(PayloadList *list, const char *directory) {
  DIR *dir = opendir(directory);
  if (!dir)
    return;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_type != DT_REG || !is_elf_filename(entry->d_name))
      continue;

    char path[512];
    char marker_path[512];
    const int path_len =
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
    const int marker_len = snprintf(marker_path, sizeof(marker_path),
                                    "%s/%s.auto_start", directory,
                                    entry->d_name);
    if (path_len <= 0 || (size_t)path_len >= sizeof(path) || marker_len <= 0 ||
        (size_t)marker_len >= sizeof(marker_path)) {
      LOG_WARN("skipping payload with oversized path: %s", entry->d_name);
      continue;
    }

    if (!if_exists(marker_path)) {
      LOG_DEBUG("skipping auto start for payload: %s", path);
      continue;
    }
    char canonical[ONION_PAYLOAD_PATH_SIZE];
    if (!onion_payload_canonical_path(path, canonical, sizeof(canonical)))
      continue;
    if (payload_list_contains_path(list, canonical)) {
      LOG_DEBUG("skipping duplicate payload path: %s", path);
      continue;
    }
    if (!payload_list_append(list, canonical, entry->d_name)) {
      LOG_ERROR("failed to record payload for auto start: %s", path);
      break;
    }
  }
  closedir(dir);
}

/*
 * <name>.elf.auto_start without <name>.elf: the user (or the NeoHEN install
 * zip) asked for a payload that is not there. Say which file and where, then
 * carry on with the rest.
 */
#define MISSING_SEEN_MAX 16

/* The same folder is reachable as /data/... and /user/data/...; report each
 * missing file name only once per boot. */
static bool missing_seen_first(const char *name) {
  static char seen[MISSING_SEEN_MAX][256];
  static size_t count;
  for (size_t i = 0; i < count; ++i)
    if (strcmp(seen[i], name) == 0)
      return false;
  if (count < MISSING_SEEN_MAX)
    snprintf(seen[count++], sizeof(seen[0]), "%s", name);
  return true;
}

static void report_missing_autostart(const char *directory) {
  DIR *dir = opendir(directory);
  if (!dir)
    return;
  static const char kSuffix[] = ".auto_start";
  const size_t suffix_len = sizeof(kSuffix) - 1;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    const size_t len = strlen(entry->d_name);
    if (len <= suffix_len ||
        strcmp(entry->d_name + len - suffix_len, kSuffix) != 0)
      continue;
    char elf_name[256];
    if (len - suffix_len >= sizeof(elf_name))
      continue;
    memcpy(elf_name, entry->d_name, len - suffix_len);
    elf_name[len - suffix_len] = '\0';
    if (!is_elf_filename(elf_name))
      continue;
    char elf_path[512];
    snprintf(elf_path, sizeof(elf_path), "%s/%s", directory, elf_name);
    if (if_exists(elf_path) || !missing_seen_first(elf_name))
      continue;
    LOG_WARN("auto start payload missing: %s", elf_path);
    bootstrap_notify("notify.payload.missing_fmt", elf_name, directory);
  }
  closedir(dir);
}

/* kstuff must be fully up before ShadowMount+; 10 s matches the manual
 * sequence verified on 13.42 (kstuff, wait 10 s, then the rest). */
#define KSTUFF_SETTLE_MS 10000u

static void wait_for_kstuff_settle(void) {
  const uint64_t since = bootstrap_ms_since_kstuff_ready();
  if (since == UINT64_MAX || since >= KSTUFF_SETTLE_MS)
    return;
  const unsigned int wait_s =
      (unsigned int)((KSTUFF_SETTLE_MS - since + 999u) / 1000u);
  LOG_INFO("Auto-start: waiting %u s for kstuff to settle", wait_s);
  unsigned int remaining = wait_s;
  while (remaining)
    remaining = sleep(remaining);
}

static int compare_payload_entries(const void *a, const void *b) {
  const PayloadEntry *first = (const PayloadEntry *)a;
  const PayloadEntry *second = (const PayloadEntry *)b;
  return onion_payload_compare(first->config.priority, first->path,
                                second->config.priority, second->path);
}

static PayloadList find_autostart_payloads(void) {
  PayloadList list = {0};
  for (size_t i = 0;
       i < sizeof(kPayloadDirectories) / sizeof(kPayloadDirectories[0]); ++i) {
    scan_payload_directory(&list, kPayloadDirectories[i]);
  }
  if (list.count > 1)
    qsort(list.entries, list.count, sizeof(*list.entries), compare_payload_entries);
  return list;
}

void bootstrap_payload_autostart(void) {
  if (!elfldr_remote_onion_available()) {
    LOG_WARN("Skipping user payload autostart: private elfldr :9020 unavailable");
    bootstrap_notify("notify.payload.autostart_skipped");
    return;
  }

  for (size_t i = 0;
       i < sizeof(kPayloadDirectories) / sizeof(kPayloadDirectories[0]); ++i)
    report_missing_autostart(kPayloadDirectories[i]);

  PayloadList list = find_autostart_payloads();
  if (list.count > 0)
    wait_for_kstuff_settle();
  int loaded = 0;
  for (size_t i = 0; i < list.count; ++i) {
    const PayloadEntry *entry = &list.entries[i];
    if (strstr(entry->filename, "elfldr") != NULL)
      continue;

    char identity[ONION_PAYLOAD_IDENTITY_SIZE];
    const char *key = onion_payload_identity_from_path(
        entry->path, identity, sizeof(identity)) ? identity : NULL;
    if (key && onion_payload_running(key))
      continue;

    LOG_INFO("Auto-start payload: %s priority=%d delay=%ds", entry->path,
             entry->config.priority, entry->config.delay_seconds);
    unsigned int remaining = (unsigned int)entry->config.delay_seconds;
    while (remaining)
      remaining = sleep(remaining);
    if (!onion_payload_load_with_key(entry->path, entry->filename, key)) {
      bootstrap_notify("notify.payload.load_failed_path", entry->path);
      LOG_ERROR("FAILED!");
      continue;
    }
    LOG_DEBUG("Loaded!");
    ++loaded;
  }

  LOG_DEBUG("Successfully loaded %d payloads", loaded);
  payload_list_destroy(&list);
}
