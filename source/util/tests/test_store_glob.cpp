/* Host tests for the store's automatic update (update_check.cpp): which
 * release file is the new one of a payload whose file name carries its
 * version. The source is included so its internal helpers can be reached;
 * it is not linked anywhere else in the host tests. */
#include "test_harness.h"

#include "../source/update_check.cpp"

/* util's main.cpp has the real one */
onion::SettingsStore g_settings;

namespace {

int test_glob() {
  /* ps5debug-NG: saved as ps5debug-NG.elf, released as ps5debug-NG_v1.3.2.elf */
  TEST_ASSERT_TRUE(glob_match("ps5debug-NG*.elf", "ps5debug-NG_v1.3.2.elf"));
  TEST_ASSERT_TRUE(glob_match("ps5debug-NG*.elf", "ps5debug-NG.elf"));
  TEST_ASSERT_TRUE(!glob_match("ps5debug-NG*.elf", "ps5debug-NG_v1.3.2.elf.sig"));
  TEST_ASSERT_TRUE(!glob_match("ps5debug-NG*.elf", "xps5debug-NG.elf"));
  TEST_ASSERT_TRUE(glob_match("a?c", "abc"));
  TEST_ASSERT_TRUE(!glob_match("a?c", "ac"));
  TEST_ASSERT_TRUE(glob_match("a**b", "ab"));
  TEST_ASSERT_TRUE(glob_match("x.elf", "x.elf"));
  TEST_ASSERT_TRUE(!glob_match("x.elf", "x.elfx"));
  TEST_ASSERT_TRUE(asset_glob_ok("ps5debug-NG*.elf"));
  TEST_ASSERT_TRUE(!asset_glob_ok("*"));
  TEST_ASSERT_TRUE(!asset_glob_ok("../x*.elf"));
  TEST_ASSERT_TRUE(!asset_glob_ok("a/b.elf"));
  TEST_ASSERT_TRUE(!asset_glob_ok(""));
  return 0;
}

} // namespace

extern "C" int test_store_glob_suite(void) {
  return onion_test_run("store_asset_glob", test_glob);
}
