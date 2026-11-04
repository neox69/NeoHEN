/* Host tests for onion/update_state.h: the rules the bootstrapper follows
 * for a NeoHEN installed from NeoWEB (hand-off, trial, rollback). */
#include "test_harness.h"

#include <onion/tree.h>
#include <onion/update_state.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void put(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (f) {
    fputs(text, f);
    fclose(f);
  }
}

static int test_versions(void) {
  TEST_ASSERT_TRUE(onion_update_version_cmp("v0.1.0-rc16", "0.1.0-rc15+ab12-dirty") > 0);
  TEST_ASSERT_TRUE(onion_update_version_cmp("0.1.0", "0.1.0-rc99") > 0);
  TEST_ASSERT_EQ_INT(0, onion_update_version_cmp("0.1.0-rc16+x", "v0.1.0-rc16"));
  TEST_ASSERT_EQ_INT(-2, onion_update_version_cmp("dirty-abcd", "0.1.0"));
  TEST_ASSERT_EQ_INT(-2, onion_update_version_cmp("0.1.0x", "0.1.0"));
  return 0;
}

static int test_decide(void) {
  onion_update_state st;
  memset(&st, 0, sizeof(st));
  onion_update_decision d = onion_update_decide(&st, "", "0.1.0-rc16", 0, true);
  TEST_ASSERT_TRUE(d.action == ONION_UPDATE_RUN_SELF && !d.count_trial);

  /* rc17 on trial, rc16 sent: hand off; the outer one does not count */
  strcpy(st.status, "trial");
  strcpy(st.current, "0.1.0-rc17");
  d = onion_update_decide(&st, "0.1.0-rc17", "0.1.0-rc16+ab", 0, true);
  TEST_ASSERT_TRUE(d.action == ONION_UPDATE_HAND_OFF && !d.count_trial && !d.confirm_ok);
  d = onion_update_decide(&st, "0.1.0-rc17", "0.1.0-rc17+cd", 0, true);
  TEST_ASSERT_TRUE(d.action == ONION_UPDATE_RUN_SELF && d.count_trial);

  st.trial_starts = 1; /* one trial start that failed */
  d = onion_update_decide(&st, "0.1.0-rc17", "0.1.0-rc16", 1, true);
  TEST_ASSERT_TRUE(d.action == ONION_UPDATE_HAND_OFF);
  d = onion_update_decide(&st, "0.1.0-rc17", "0.1.0-rc16", 0, true); /* it held */
  TEST_ASSERT_TRUE(d.confirm_ok && d.action == ONION_UPDATE_HAND_OFF);
  /* failures from before the trial are not the trial's */
  d = onion_update_decide(&st, "0.1.0-rc17", "0.1.0-rc16", 3, true);
  TEST_ASSERT_TRUE(d.action == ONION_UPDATE_HAND_OFF);

  st.trial_starts = 2; /* two in a row */
  d = onion_update_decide(&st, "0.1.0-rc17", "0.1.0-rc16", 2, true);
  TEST_ASSERT_TRUE(d.action == ONION_UPDATE_ROLL_BACK);
  d = onion_update_decide(&st, "0.1.0-rc17", "0.1.0-rc17", 2, true);
  TEST_ASSERT_TRUE(d.action == ONION_UPDATE_ROLL_BACK);

  strcpy(st.status, "ok"); /* confirmed: later crashes are safe mode's */
  d = onion_update_decide(&st, "0.1.0-rc17", "0.1.0-rc16", 4, true);
  TEST_ASSERT_TRUE(d.action == ONION_UPDATE_HAND_OFF && !d.confirm_ok);
  d = onion_update_decide(&st, "0.1.0-rc17", "0.1.0-rc18", 0, true); /* newer sent */
  TEST_ASSERT_TRUE(d.action == ONION_UPDATE_RUN_SELF && !d.count_trial);
  d = onion_update_decide(&st, "0.1.0-rc17", "dirty-ab", 0, true); /* developer build */
  TEST_ASSERT_TRUE(d.action == ONION_UPDATE_RUN_SELF);
  /* update.apply_at_boot off: the sent one runs, nothing is decided for the
   * installed one, and a trial's count starts over */
  strcpy(st.status, "trial");
  st.trial_starts = 2;
  d = onion_update_decide(&st, "0.1.0-rc17", "0.1.0-rc16", 2, false);
  TEST_ASSERT_TRUE(d.action == ONION_UPDATE_RUN_SELF && d.held && d.reset_trial &&
                   !d.confirm_ok && !d.count_trial);
  d = onion_update_decide(&st, "0.1.0-rc17", "0.1.0-rc16", 0, false);
  TEST_ASSERT_TRUE(d.held && !d.confirm_ok);
  /* "Restart now" sent it: it runs and counts as usual */
  st.trial_starts = 0;
  d = onion_update_decide(&st, "0.1.0-rc17", "0.1.0-rc17", 0, false);
  TEST_ASSERT_TRUE(!d.held && d.count_trial && d.action == ONION_UPDATE_RUN_SELF);
  st.trial_starts = 2;
  d = onion_update_decide(&st, "0.1.0-rc17", "0.1.0-rc17", 2, false);
  TEST_ASSERT_TRUE(d.action == ONION_UPDATE_ROLL_BACK);
  strcpy(st.status, "ok");
  d = onion_update_decide(&st, "0.1.0-rc17", "0.1.0-rc16", 0, false);
  TEST_ASSERT_TRUE(d.held && !d.reset_trial);
  d = onion_update_decide(&st, "0.1.0-rc17", "0.1.0-rc18", 0, false); /* newer sent */
  TEST_ASSERT_TRUE(!d.held && d.action == ONION_UPDATE_RUN_SELF);
  /* current/ that state.json does not describe: on trial from 0 */
  strcpy(st.current, "0.1.0-rc16");
  d = onion_update_decide(&st, "0.1.0-rc17", "0.1.0-rc17", 3, true);
  TEST_ASSERT_TRUE(d.count_trial && d.action == ONION_UPDATE_RUN_SELF);
  return 0;
}

static int test_state_and_rollback(void) {
  const char *dir = ONION_DATA_ROOT "/update-state";
  struct stat sb;
  rmtree(dir);
  TEST_ASSERT_TRUE(mkdir_tree(ONION_DATA_ROOT "/update-state/current"));
  TEST_ASSERT_TRUE(mkdir_tree(ONION_DATA_ROOT "/update-state/previous"));
  put(ONION_DATA_ROOT "/update-state/current/version.txt", "0.1.0-rc17\n");
  put(ONION_DATA_ROOT "/update-state/previous/version.txt", "0.1.0-rc16\n");
  onion_update_state st;
  memset(&st, 0, sizeof(st));
  strcpy(st.status, "trial");
  strcpy(st.current, "0.1.0-rc17");
  strcpy(st.previous, "0.1.0-rc16");
  strcpy(st.from, "0.1.0-rc16+x");
  st.trial_starts = 2;
  st.installed_at = 1234;
  TEST_ASSERT_TRUE(onion_update_state_save(dir, &st));
  onion_update_state b;
  TEST_ASSERT_TRUE(onion_update_state_load(dir, &b));
  TEST_ASSERT_TRUE(!strcmp(b.status, "trial") && !strcmp(b.current, "0.1.0-rc17"));
  TEST_ASSERT_TRUE(b.trial_starts == 2 && b.installed_at == 1234 && !strcmp(b.from, "0.1.0-rc16+x"));

  TEST_ASSERT_TRUE(onion_update_roll_back(dir, &b, "auto", 99));
  TEST_ASSERT_TRUE(!strcmp(b.status, "ok") && !strcmp(b.current, "0.1.0-rc16"));
  TEST_ASSERT_TRUE(!strcmp(b.rolled_back_from, "0.1.0-rc17") && b.rolled_back_at == 99);
  char w[ONION_UPDATE_VER_MAX];
  onion_update_read_word(ONION_DATA_ROOT "/update-state/current/version.txt", w, sizeof(w));
  TEST_ASSERT_TRUE(!strcmp(w, "0.1.0-rc16"));
  TEST_ASSERT_TRUE(stat(ONION_DATA_ROOT "/update-state/previous", &sb) != 0);
  TEST_ASSERT_TRUE(stat(ONION_DATA_ROOT "/update-state/failed", &sb) != 0);

  /* no previous/ left: current/ goes, the NeoHEN.elf that is sent runs */
  TEST_ASSERT_TRUE(onion_update_roll_back(dir, &b, "manual", 100));
  TEST_ASSERT_TRUE(!strcmp(b.status, "rolled_back") && !b.current[0]);
  TEST_ASSERT_TRUE(stat(ONION_DATA_ROOT "/update-state/current", &sb) != 0);
  onion_update_state c;
  onion_update_state_load(dir, &c);
  TEST_ASSERT_TRUE(!strcmp(c.status, "rolled_back") && !strcmp(c.reason, "manual"));
  TEST_ASSERT_TRUE(onion_update_decide(&c, "", "0.1.0-rc16", 0, true).action == ONION_UPDATE_RUN_SELF);
  rmtree(dir);
  return 0;
}

int test_update_state_suite(void) {
  int failures = 0;
  failures += onion_test_run("update_state_versions", test_versions);
  failures += onion_test_run("update_state_decide", test_decide);
  failures += onion_test_run("update_state_rollback", test_state_and_rollback);
  return failures;
}
