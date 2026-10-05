/* Copyright (C) 2026 OnionHEN / LightningMods */

#include <onion/brand.h>
#include "bootstrap_assets.h"

#include <onion/fs.h>
#include <onion/log.h>
#include <onion/notify.h>
#include <onion/topnav_icon.h>

#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

extern uint8_t sicon_start[];
extern const unsigned int sicon_size;

#define ONION_ICON(name)                                                       \
  extern uint8_t name##_start[];                                               \
  extern const unsigned int name##_size;
#include "icon_manifest.inc"
#undef ONION_ICON

typedef struct EmbeddedIcon {
  const char *name;
  const uint8_t *data;
  const unsigned int *size;
} EmbeddedIcon;

static const EmbeddedIcon kEmbeddedIcons[] = {
#define ONION_ICON(name) {#name, name##_start, &name##_size},
#include "icon_manifest.inc"
#undef ONION_ICON
};

bool bootstrap_assets_write(bool system_ex_icon) {
  mkdir(ONION_DATA_DIR, 0777);
  mkdir(ONION_DATA_DIR "/assets", 0777);
  mkdir("/user/data", 0777);
  mkdir(ONION_USER_DATA_DIR, 0777);

  const bool startup_icon_ready = write_file_atomic(
      ONION_NOTIFY_ICON_PATH, sicon_start, sicon_size);

  for (size_t i = 0; i < sizeof(kEmbeddedIcons) / sizeof(kEmbeddedIcons[0]);
       ++i) {
    char path[256];
    snprintf(path, sizeof(path), ONION_DATA_DIR "/assets/%s.png",
             kEmbeddedIcons[i].name);
    (void)write_file_atomic(path, kEmbeddedIcons[i].data,
                            *kEmbeddedIcons[i].size);
  }

  char topnav_icon[ONION_TOPNAV_ICON_PATH_LEN + 1];
  onion_topnav_icon_path(system_ex_icon, topnav_icon);
  if (system_ex_icon) {
    LOG_WARN("home_screen.system_ex_icon=true: writing %s", topnav_icon);
    mkdir("/system_ex/vsh_asset", 0777);
  } else {
    mkdir(ONION_RUNTIME_DIR, 0777);
  }
  if (!write_file_atomic(topnav_icon, sicon_start, sicon_size))
    LOG_WARN("top-nav icon not written: %s", topnav_icon);
  return startup_icon_ready;
}
