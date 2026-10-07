/* Copyright (C) 2025 OnionHEN / LightningMods — OnPress misc (kstuff, account, credits) */
#include <onion/brand.h>
#include "onpress.hpp"
#include "account_activator.h"
#include "neo_actions.hpp"
#include "progress_dialog.hpp"
#include "onion_cjson.hpp"

#include <cstring>
#include <unistd.h>

OnPressResult onpress_kstuff_autoload(OnPressContext &ctx) {
  const bool enabled = value_as_int(ctx);
  if (enabled == g_settings.kstuff_autoload)
    return OnPressResult::EarlyReturn;

  g_settings.kstuff_autoload = enabled;
  if (enabled) {
    unlink(ONION_USER_DATA_DIR "/no_kstuff");
    unlink(ONION_DATA_DIR "/no_kstuff");
    notify("notify.kstuff.next_boot_on");
  } else {
    touch_file(ONION_USER_DATA_DIR "/no_kstuff");
    touch_file(ONION_DATA_DIR "/no_kstuff");
    notify("notify.kstuff.next_boot_off");
  }
  return OnPressResult::Handled;
}

static OnPressResult id_download_cheats(OnPressContext &ctx) {
  ctx.dirty = false;
  std::string reply;
  if (!IPC_Client::getInstance(true).DownloadCheats(nullptr, nullptr, reply)) {
    notify("notify.cheats.sync.error", "ipc");
    return OnPressResult::Consumed;
  }
  onion_cjson::Root response(reply);
  const char *state = response && cJSON_IsObject(response.get())
                          ? onion_cjson::string_item(response.get(), "state")
                          : nullptr;
  if (!state) {
    notify("notify.cheats.sync.error", "ipc_response");
    return OnPressResult::Consumed;
  }
  const int task_id = onion_cjson::int_item(response.get(), "task_id", 0);
  if (std::strcmp(state, "already_running") == 0) {
    notify("notify.cheats.sync.busy");
  }
  /* Button confirmation has completed before this handler runs. */
  cheat_progress_show(task_id > 0 ? static_cast<uint32_t>(task_id) : 0);
  if (!cheat_progress_open_page()) {
    notify("notify.cheats.sync.error", "navigation");
  }
  return OnPressResult::Consumed;
}

OnPressResult onpress_delete_kstuff(OnPressContext &ctx) {
  (void)ctx;
  unlink(ONION_USER_DATA_DIR "/kstuff.elf");
  notify("notify.kstuff.deleted");
  return OnPressResult::Handled;
}

static OnPressResult id_activate_account(OnPressContext &ctx) {
  ctx.dirty = false;
  Activator activator(true);
  if (!activator.Valid()) {
    notify("notify.account.invalid");
    return OnPressResult::Consumed;
  }
  if (!activator.IsNotActivated()) {
    notify("notify.account.already");
    return OnPressResult::Consumed;
  }
  if (!activator.Activate()) {
    notify("notify.account.failed");
    return OnPressResult::Consumed;
  }
  notify("notify.account.activated");
  return OnPressResult::Consumed;
}

static OnPressResult id_lm_test(OnPressContext &ctx) {
  (void)ctx;
  LOG_DEBUG("LM's Test Button Pressed");
  return OnPressResult::Handled;
}

static OnPressResult id_onionhen_credits(OnPressContext &ctx) {
  (void)ctx;
  return OnPressResult::EarlyReturn;
}

static OnPressResult id_presentation_card(OnPressContext &ctx) {
  // Author and donator cards in the dynamic Toolbox XML are display-only.
  ctx.dirty = false;
  return OnPressResult::Consumed;
}

static OnPressResult id_smp_relaunch(OnPressContext &ctx) {
  /* The confirmation has been accepted before this handler runs. */
  ctx.dirty = false;
  neo_smp_relaunch();
  return OnPressResult::Consumed;
}
static OnPressResult id_save_backup(OnPressContext &ctx) {
  ctx.dirty = false;
  neo_saves_backup_now();
  return OnPressResult::Consumed;
}
static OnPressResult id_save_restore(OnPressContext &ctx) {
  /* id_save_restore_<n>; the confirmation has already been accepted. */
  ctx.dirty = false;
  neo_saves_restore(ctx.id.back() - '0');
  return OnPressResult::Consumed;
}
static OnPressResult id_theme(OnPressContext &ctx) {
  /* id_theme_stock, or id_theme_<n> */
  ctx.dirty = false;
  neo_theme_set(ctx.id == "id_theme_stock" ? -1 : ctx.id.back() - '0');
  return OnPressResult::Consumed;
}
static OnPressResult id_theme_restart(OnPressContext &ctx) {
  /* The confirmation has been accepted before this handler runs. */
  ctx.dirty = false;
  neo_menu_restart();
  return OnPressResult::Consumed;
}
static const OnPressExactEntry kRootExact[] = {
    {"id_theme_restart", id_theme_restart},
    {"id_smp_relaunch", id_smp_relaunch},
    {"id_theme_stock", id_theme},
    {"id_theme_0", id_theme},
    {"id_theme_1", id_theme},
    {"id_theme_2", id_theme},
    {"id_theme_3", id_theme},
    {"id_theme_4", id_theme},
    {"id_theme_5", id_theme},
    {"id_theme_6", id_theme},
    {"id_theme_7", id_theme},
    {"id_save_backup", id_save_backup},
    {"id_save_restore_0", id_save_restore},
    {"id_save_restore_1", id_save_restore},
    {"id_save_restore_2", id_save_restore},
    {"id_save_restore_3", id_save_restore},
    {"id_save_restore_4", id_save_restore},
    {"id_save_restore_5", id_save_restore},
    {"id_save_restore_6", id_save_restore},
    {"id_save_restore_7", id_save_restore},
    {"id_download_cheats", id_download_cheats},
    {"id_lm_test", id_lm_test},
    {"id_onionhen_credits", id_onionhen_credits},
    {"id_author_0xp0co", id_presentation_card},
    {"id_donator_aglx", id_presentation_card},
    {"id_donator_ljf", id_presentation_card},
    {"id_donator_szx", id_presentation_card},
    {"id_donator_xiaomao", id_presentation_card},
    {"id_donator_xie", id_presentation_card},
};

const OnPressExactEntry *onpress_misc_root_exact(size_t *count) {
  *count = sizeof(kRootExact) / sizeof(kRootExact[0]);
  return kRootExact;
}

static const OnPressExactEntry kAccountExact[] = {
    {"id_activate_account", id_activate_account},
};

const OnPressExactEntry *onpress_account_exact(size_t *count) {
  *count = sizeof(kAccountExact) / sizeof(kAccountExact[0]);
  return kAccountExact;
}
