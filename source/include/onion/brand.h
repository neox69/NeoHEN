/*
 * Product branding: the visible name and the on-console directories.
 *
 * The defaults reproduce OnionHEN exactly. Forks change them through the
 * CMake cache variables ONION_BRAND_NAME, ONION_DIR_NAME and
 * ONION_DIR_NAME_LOWER (see source/CMakeLists.txt), which arrive here as
 * compile definitions. Locale strings use the matching {brand} / {data_dir}
 * placeholders (see source/i18n/README.md).
 */
#pragma once

#ifndef ONION_BRAND_NAME
#define ONION_BRAND_NAME "OnionHEN"
#endif

/* Directory name under /data and on USB drives. */
#ifndef ONION_DIR_NAME
#define ONION_DIR_NAME "OnionHEN"
#endif

/* Lower-case spelling, used for the runtime directory and legacy paths. */
#ifndef ONION_DIR_NAME_LOWER
#define ONION_DIR_NAME_LOWER "onionhen"
#endif

/* Persistent data, as seen by the daemons. */
#define ONION_DATA_DIR "/data/" ONION_DIR_NAME
/* The same directory as seen from the SceShellUI sandbox. */
#define ONION_USER_DATA_DIR "/user" ONION_DATA_DIR
/* Volatile runtime state (IPC sockets, ready flags). */
#define ONION_RUNTIME_DIR "/system_tmp/" ONION_DIR_NAME_LOWER
