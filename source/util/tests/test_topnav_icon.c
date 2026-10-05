/* Top-nav icon path: must always fill the 33-byte HomeUI string exactly. */

#include "test_harness.h"

#include <onion/topnav_icon.h>

#include <string.h>

static int test_runtime_path_by_default(void) {
  char path[ONION_TOPNAV_ICON_PATH_LEN + 1];
  onion_topnav_icon_path(false, path);
  TEST_ASSERT_TRUE(strlen(path) == ONION_TOPNAV_ICON_PATH_LEN);
  TEST_ASSERT_TRUE(strncmp(path, ONION_RUNTIME_DIR "/icon",
                           strlen(ONION_RUNTIME_DIR "/icon")) == 0);
  TEST_ASSERT_TRUE(strcmp(path + ONION_TOPNAV_ICON_PATH_LEN - 4, ".png") == 0);
  TEST_ASSERT_TRUE(strstr(path, "/system_ex") == NULL);
  return 0;
}

static int test_system_ex_only_when_enabled(void) {
  char path[ONION_TOPNAV_ICON_PATH_LEN + 1];
  onion_topnav_icon_path(true, path);
  TEST_ASSERT_TRUE(strcmp(path, ONION_TOPNAV_ICON_LEGACY_PATH) == 0);
  TEST_ASSERT_TRUE(strlen(ONION_TOPNAV_ICON_LEGACY_PATH) ==
                   ONION_TOPNAV_ICON_PATH_LEN);
  return 0;
}

int test_topnav_icon_suite(void) {
  int failures = 0;
  failures += onion_test_run("topnav_icon.runtime_default",
                             test_runtime_path_by_default);
  failures += onion_test_run("topnav_icon.system_ex_opt_in",
                             test_system_ex_only_when_enabled);
  return failures;
}
