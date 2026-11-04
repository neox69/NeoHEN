/* Host tests for the app store's homebrew-folder zips (app_store.cpp):
 * where the files are found in the zip, what is refused, and what lands in
 * the stage folder. The source is included so its internal helpers can be
 * reached; it is not linked anywhere else in the host tests. */
#include "test_harness.h"

#include "../source/app_store.cpp"

#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

using Entry = std::pair<const char *, const char *>;

std::string make_zip(const char *name, const std::vector<Entry> &files) {
  const std::string path = std::string("/tmp/onion-appstore-") + name + ".zip";
  mz_zip_archive zip{};
  unlink(path.c_str());
  if (!mz_zip_writer_init_file(&zip, path.c_str(), 0))
    return "";
  for (const auto &f : files) {
    if (!mz_zip_writer_add_mem(&zip, f.first, f.second, strlen(f.second), MZ_BEST_SPEED)) {
      mz_zip_writer_end(&zip);
      return "";
    }
  }
  const bool ok = mz_zip_writer_finalize_archive(&zip);
  mz_zip_writer_end(&zip);
  return ok ? path : "";
}

bool exists(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

const char *kStage = "/tmp/onion-appstore-stage";

/* OffAct.zip's layout: everything at the top */
int test_homebrew_top() {
  const std::string z = make_zip("top", {{"OffAct.elf", "\x7f" "ELF"},
                                         {"homebrew.js", "async function main(){}"},
                                         {"sce_sys/icon0.png", "png"}});
  TEST_ASSERT_TRUE(!z.empty());
  rmtree(kStage);
  TEST_ASSERT_STREQ("", unpack(z, "OffAct", true, kStage).c_str());
  TEST_ASSERT_TRUE(exists(std::string(kStage) + "/OffAct.elf"));
  TEST_ASSERT_TRUE(exists(std::string(kStage) + "/homebrew.js"));
  TEST_ASSERT_TRUE(exists(std::string(kStage) + "/sce_sys/icon0.png"));
  rmtree(kStage);
  unlink(z.c_str());
  return 0;
}

/* the same inside one folder: that folder becomes /data/homebrew/<dir> */
int test_homebrew_one_folder() {
  const std::string z = make_zip("folder", {{"OffAct/eboot.elf", "\x7f" "ELF"},
                                            {"OffAct/sce_sys/icon0.png", "png"}});
  TEST_ASSERT_TRUE(!z.empty());
  rmtree(kStage);
  TEST_ASSERT_STREQ("", unpack(z, "OffAct", true, kStage).c_str());
  TEST_ASSERT_TRUE(exists(std::string(kStage) + "/eboot.elf"));
  TEST_ASSERT_TRUE(!exists(std::string(kStage) + "/OffAct"));
  rmtree(kStage);
  unlink(z.c_str());
  return 0;
}

int test_homebrew_refused() {
  /* nothing websrv would list */
  std::string z = make_zip("none", {{"readme.txt", "x"}, {"payload.elf", "x"}});
  rmtree(kStage);
  TEST_ASSERT_TRUE(!unpack(z, "Foo", true, kStage).empty());
  TEST_ASSERT_TRUE(!exists(std::string(kStage) + "/readme.txt"));
  unlink(z.c_str());
  /* two folders: not one homebrew */
  z = make_zip("two", {{"A/homebrew.js", "x"}, {"B/homebrew.js", "x"}});
  TEST_ASSERT_TRUE(!unpack(z, "Foo", true, kStage).empty());
  unlink(z.c_str());
  /* zip-slip: one unsafe name refuses the whole zip, before anything lands */
  const char *bad[] = {"../evil.js", "sce_sys/../../evil.js", "./x", "a\\b"};
  for (const char *b : bad) {
    rmtree(kStage);
    z = make_zip("slip", {{"homebrew.js", "x"}, {b, "x"}});
    TEST_ASSERT_TRUE(!z.empty());
    TEST_ASSERT_TRUE(!unpack(z, "Foo", true, kStage).empty());
    TEST_ASSERT_TRUE(!exists(std::string(kStage) + "/homebrew.js"));
    TEST_ASSERT_TRUE(!exists("/tmp/evil.js"));
    unlink(z.c_str());
  }
  rmtree(kStage);
  return 0;
}

int test_homebrew_dir_names() {
  TEST_ASSERT_TRUE(dir_ok("OffAct"));
  TEST_ASSERT_TRUE(dir_ok("my-app_1.2"));
  TEST_ASSERT_TRUE(!dir_ok(""));
  TEST_ASSERT_TRUE(!dir_ok(".hidden"));
  TEST_ASSERT_TRUE(!dir_ok(".."));
  TEST_ASSERT_TRUE(!dir_ok("a/b"));
  TEST_ASSERT_TRUE(!dir_ok("a b"));
  TEST_ASSERT_TRUE(!dir_ok("PPSA12345")); /* ShadowMount+'s, not a homebrew folder */
  TEST_ASSERT_TRUE(!dir_ok(std::string(65, 'a')));
  return 0;
}

/* an app zip still needs its title id folder with eboot.bin and param.json */
int test_app_zip_unchanged() {
  const std::string z = make_zip("app", {{"PPSA12345/eboot.bin", "x"},
                                         {"PPSA12345/sce_sys/param.json", "{}"},
                                         {"LICENSE", "x"}});
  rmtree(kStage);
  TEST_ASSERT_STREQ("", unpack(z, "PPSA12345", false, kStage).c_str());
  TEST_ASSERT_TRUE(exists(std::string(kStage) + "/eboot.bin"));
  TEST_ASSERT_TRUE(!exists(std::string(kStage) + "/LICENSE"));
  rmtree(kStage);
  /* and a homebrew zip is not an app */
  const std::string h = make_zip("hb", {{"homebrew.js", "x"}});
  TEST_ASSERT_TRUE(!unpack(h, "PPSA12345", false, kStage).empty());
  rmtree(kStage);
  unlink(z.c_str());
  unlink(h.c_str());
  return 0;
}

} // namespace

extern "C" int test_app_store_suite(void) {
  int failures = 0;
  failures += onion_test_run("app_store_homebrew_top", test_homebrew_top);
  failures += onion_test_run("app_store_homebrew_one_folder", test_homebrew_one_folder);
  failures += onion_test_run("app_store_homebrew_refused", test_homebrew_refused);
  failures += onion_test_run("app_store_homebrew_dir_names", test_homebrew_dir_names);
  failures += onion_test_run("app_store_app_zip", test_app_zip_unchanged);
  return failures;
}
