/*
 * Path of the Toolbox icon shown in the HomeUI top navigation bar.
 *
 * The HomeUI patch writes this path over a 33-byte string inside the
 * SceShellUI bundle, so every candidate must be exactly that long.
 *
 * Default: a file under the runtime directory (/system_tmp, volatile), which
 * disappears on reboot just like the in-memory ShellUI patch. The legacy
 * location under /system_ex is persistent and is only used when the user
 * enables home_screen.system_ex_icon.
 */
#pragma once

#include <onion/brand.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define ONION_TOPNAV_ICON_PATH_LEN 33
#define ONION_TOPNAV_ICON_LEGACY_PATH "/system_ex/vsh_asset/onionhen.png"

#ifdef __cplusplus
static_assert(sizeof(ONION_TOPNAV_ICON_LEGACY_PATH) - 1 ==
                  ONION_TOPNAV_ICON_PATH_LEN,
              "legacy icon path must fill the patched string exactly");
#endif

/*
 * Runtime-directory icon path padded to ONION_TOPNAV_ICON_PATH_LEN, e.g.
 * "/system_tmp/neohen/icon______.png". Returns false if the runtime directory
 * name is too long to fit.
 */
static inline bool
onion_topnav_icon_runtime_path(char out[ONION_TOPNAV_ICON_PATH_LEN + 1]) {
  static const char kPrefix[] = ONION_RUNTIME_DIR "/icon";
  static const char kSuffix[] = ".png";
  const size_t prefix_len = sizeof(kPrefix) - 1;
  const size_t suffix_len = sizeof(kSuffix) - 1;
  if (prefix_len + suffix_len > ONION_TOPNAV_ICON_PATH_LEN)
    return false;
  memcpy(out, kPrefix, prefix_len);
  memset(out + prefix_len, '_',
         ONION_TOPNAV_ICON_PATH_LEN - prefix_len - suffix_len);
  memcpy(out + ONION_TOPNAV_ICON_PATH_LEN - suffix_len, kSuffix, suffix_len);
  out[ONION_TOPNAV_ICON_PATH_LEN] = '\0';
  return true;
}

/* The path the HomeUI patch and the bootstrapper agree on. */
static inline void
onion_topnav_icon_path(bool system_ex_icon,
                       char out[ONION_TOPNAV_ICON_PATH_LEN + 1]) {
  if (system_ex_icon || !onion_topnav_icon_runtime_path(out))
    snprintf(out, ONION_TOPNAV_ICON_PATH_LEN + 1, "%s",
             ONION_TOPNAV_ICON_LEGACY_PATH);
}
