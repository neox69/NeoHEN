/* Copyright (C) 2025 OnionHEN / LightningMods
 *
 * Remote Play pairing page backed by the native PS5 Remote Play service.
 */

#include <onion/brand.h>
#include <ctime>
#include "account_activator.h"
#include "external_symbols.hpp"
#include "hooked_funcs.hpp"
#include "ps5_settings_ui.hpp"
#include "remote_play.hpp"
#include "toolbox_i18n.hpp"
#include "onion_cjson.hpp"
#include <unistd.h>

#include <onion/notify.h>

#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <sstream>
#include <string>

namespace {

constexpr int kRemotePlayEnableRegistry = 0x41810000;
constexpr char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::atomic<remote_play::PairingState> g_pairing_state{
    remote_play::PairingState::Idle};
bool g_confirm_thread_created = false;
pthread_t g_confirm_thread{};
MonoObject *g_remote_play_page = nullptr;
uint32_t g_remote_play_page_handle = 0;

void release_remote_play_page() {
  if (g_remote_play_page_handle && mono_gchandle_free)
    mono_gchandle_free(g_remote_play_page_handle);
  g_remote_play_page = nullptr;
  g_remote_play_page_handle = 0;
}

void invalidate_pin_registration(const char *reason) {
  if (!sceRemoteplayNotifyPinCodeError) {
    LOG_ERROR("remote_play: pin invalidation unavailable (%s)", reason);
    return;
  }

  const int error = sceRemoteplayNotifyPinCodeError(1);
  if (error != 0) {
    LOG_WARN("remote_play: pin invalidation failed error=0x%X (%s)", error,
             reason);
  } else {
    LOG_DEBUG("remote_play: pin registration invalidated (%s)", reason);
  }
}

void base64_encode(uint64_t input, char *output, size_t output_size) {
  if (!output || output_size < 13)
    return;

  unsigned char bytes[8]{};
  for (int i = 0; i < 8; ++i)
    bytes[i] = static_cast<unsigned char>((input >> (i * 8)) & 0xff);

  int out = 0;
  for (int i = 0; i < 8;) {
    const uint32_t a = bytes[i++];
    const uint32_t b = i < 8 ? bytes[i++] : 0;
    const uint32_t c = i < 8 ? bytes[i++] : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;
    output[out++] = kBase64Table[(triple >> 18) & 0x3f];
    output[out++] = kBase64Table[(triple >> 12) & 0x3f];
    output[out++] = kBase64Table[(triple >> 6) & 0x3f];
    output[out++] = kBase64Table[triple & 0x3f];
  }
  output[11] = '=';
  output[12] = '\0';
}

void *confirm_registration_loop(void *) {
  int pair_status = -1;
  int pair_error = -1;
  int last_status = -1;
  int last_pair_error = -1;

  while (g_pairing_state.load(std::memory_order_acquire) ==
         remote_play::PairingState::Waiting) {
    if (!sceRemoteplayConfirmDeviceRegist) {
      if (remote_play::try_finish_pairing(
              g_pairing_state, remote_play::PairingState::Failed)) {
        onion_notify_debug(/*show_watermark=*/0, "notify.remote_play.unavailable");
        invalidate_pin_registration("service_unavailable");
      }
      break;
    }

    const int error = sceRemoteplayConfirmDeviceRegist(&pair_status, &pair_error);
    if (pair_status != last_status || pair_error != last_pair_error) {
      LOG_DEBUG("Remote Play confirmation: error=0x%X status=%d pair_error=0x%X",
                error, pair_status, pair_error);
      last_status = pair_status;
      last_pair_error = pair_error;
    }
    if (error) {
      if (remote_play::try_finish_pairing(
              g_pairing_state, remote_play::PairingState::Failed)) {
        onion_notify_debug(/*show_watermark=*/0, "notify.remote_play.pairing_failed_fmt", error);
        invalidate_pin_registration("confirmation_failed");
      }
      break;
    }
    if (pair_status == 2) {
      if (remote_play::try_finish_pairing(
              g_pairing_state, remote_play::PairingState::Paired)) {
        onion_notify_debug(/*show_watermark=*/0, "notify.remote_play.paired");
        invalidate_pin_registration("pairing_succeeded");
      }
      break;
    }
  }

  return nullptr;
}

void join_confirm_registration_thread() {
  if (g_confirm_thread_created) {
    pthread_join(g_confirm_thread, nullptr);
    g_confirm_thread_created = false;
  }
}

uint32_t generate_pin_code() {
  if (!sceRemoteplayNotifyPinCodeError || !sceRemoteplayGeneratePinCode)
    return 0;

  (void)remote_play::try_finish_pairing(
      g_pairing_state, remote_play::PairingState::Cancelled);
  invalidate_pin_registration("new_pairing");
  join_confirm_registration_thread();

  uint32_t pin = 0;
  if (sceRemoteplayGeneratePinCode(&pin) != 0)
    return 0;

  g_pairing_state.store(remote_play::PairingState::Waiting,
                        std::memory_order_release);
  if (pthread_create(&g_confirm_thread, nullptr, confirm_registration_loop,
                     nullptr) != 0) {
    g_pairing_state.store(remote_play::PairingState::Failed,
                          std::memory_order_release);
    onion_notify_debug(/*show_watermark=*/0, "notify.remote_play.pairing_start_failed");
    invalidate_pin_registration("thread_create_failed");
    return 0;
  }
  g_confirm_thread_created = true;
  onion_notify_debug(/*show_watermark=*/0, "notify.remote_play.waiting");
  return pin;
}

void initialize_remote_play() {
  if (!sceRemoteplayInitialize) {
    onion_notify_debug(/*show_watermark=*/0, "notify.remote_play.unavailable");
    return;
  }

  int enabled = 0;
  const int read_error = sceRegMgrGetInt_hook(kRemotePlayEnableRegistry, &enabled);
  if (read_error != 0) {
    onion_notify_debug(/*show_watermark=*/0, "notify.remote_play.read_failed_fmt", read_error);
  } else if (enabled != 1) {
    const int write_error = sceRegMgrSetInt(kRemotePlayEnableRegistry, 1);
    if (write_error != 0)
      onion_notify_debug(/*show_watermark=*/0, "notify.remote_play.enable_failed_fmt", write_error);
  }

  const int error = sceRemoteplayInitialize(nullptr, 0);
  if (error != 0)
    LOG_DEBUG("sceRemoteplayInitialize returned 0x%X; continuing because "
              "the native service may already be initialized",
              error);
}

} // namespace

std::string g_remote_play_info;

void remote_play_bind_page(MonoObject *page) {
  if (!page || page == g_remote_play_page)
    return;

  release_remote_play_page();
  if (!mono_gchandle_new)
    return;

  g_remote_play_page_handle = mono_gchandle_new(page, 1);
  if (!g_remote_play_page_handle) {
    LOG_ERROR("remote_play_xml: failed to root page");
    return;
  }
  g_remote_play_page = page;
}

bool remote_play_handle_popping(MonoObject *outgoing) {
  if (!outgoing || outgoing != g_remote_play_page)
    return false;

  release_remote_play_page();
  const bool cancelled = remote_play::try_finish_pairing(
      g_pairing_state, remote_play::PairingState::Cancelled);
  if (cancelled) {
    onion_notify_debug(/*show_watermark=*/0, "notify.remote_play.pairing_cancelled");
    invalidate_pin_registration("page_popped");
  }
  join_confirm_registration_thread();
  return true;
}

void generate_remote_play_xml(std::string &xml_buffer) {
  char account_id[16]{};
  uint64_t decoded_account_id = 0;
  ps5ui::Page page("remote_play_pin_display",
                   toolbox_i18n::tr("remote_play.details.title"));
  page.root_style(ps5ui::Style::Center);

  Activator activator(true);
  if (!activator.Valid() || activator.IsNotActivated()) {
    const char *message = !activator.Valid()
                              ? toolbox_i18n::tr("account.error.read")
                              : toolbox_i18n::tr("account.status.not_activated");
    page.label("id_remote_play_account_required", message,
               ps5ui::Style::Center);
    xml_buffer = page.build();
    return;
  }

  static bool initialized = false;
  if (!initialized) {
    initialize_remote_play();
    initialized = true;
  }

  base64_encode(activator.currentUser.accountID, account_id,
                sizeof(account_id));
  decoded_account_id = activator.currentUser.accountID;
  const uint32_t pin = generate_pin_code();

  const std::string pin_text = toolbox_i18n::format(
      "remote_play.pin_fmt", pin / 10000, pin % 10000);
  std::ostringstream details;
  details << toolbox_i18n::format("remote_play.account_fmt", account_id)
          << "\n"
          << toolbox_i18n::format("remote_play.decoded_account_fmt",
                                  decoded_account_id)
          << "\n"
          << pin_text;
  g_remote_play_info = details.str();
  /* NeoHEN: NeoWEB shows these on the PC, to paste into Chiaki or PXPlay. */
  remote_play_web_note_pin(pin);

  page.label("id_remote_play_pin", pin_text, ps5ui::Style::Center)
      .label("id_remote_play_account",
             toolbox_i18n::format("remote_play.account_fmt", account_id),
             ps5ui::Style::Center);
  if (usbpath() != -1)
    page.button("id_save_rp_info", toolbox_i18n::tr("remote_play.save"),
                std::nullopt, std::nullopt, std::nullopt,
                ps5ui::Style::Center);
  xml_buffer = page.build();
}

/* ------------------------------------------------------------------------
 * NeoHEN: Remote Play from NeoWEB.
 *
 * Only SceShellUI can read and write the accounts and ask for a pairing PIN,
 * so NeoWEB leaves a request in web/rp_request.json and this thread, started
 * with the Toolbox, carries it out and keeps web/remoteplay.json up to date:
 * every user with its account ID (base64 as Chiaki and PXPlay want it, and in
 * decimal), the PIN and how the pairing is going. */
namespace {
constexpr const char *kRpRequest = ONION_USER_DATA_DIR "/web/rp_request.json";
constexpr const char *kRpStatus = ONION_USER_DATA_DIR "/web/remoteplay.json";
constexpr int kPinSeconds = 300; /* a PIN not used by then is withdrawn */

uint32_t g_web_pin = 0;
time_t g_web_pin_time = 0;
std::string g_last_op, g_last_error;
bool g_last_ok = true;

const char *state_name(remote_play::PairingState s) {
  switch (s) {
  case remote_play::PairingState::Waiting: return "waiting";
  case remote_play::PairingState::Paired: return "paired";
  case remote_play::PairingState::Failed: return "failed";
  case remote_play::PairingState::Cancelled: return "cancelled";
  default: return "idle";
  }
}

void write_status() {
  Activator activator(true);
  int fg_user = -1;
  sceUserServiceGetForegroundUser(&fg_user);
  cJSON *root = cJSON_CreateObject();
  cJSON *users = cJSON_AddArrayToObject(root, "users");
  for (int32_t slot = 1; slot <= 16; ++slot) {
    int32_t user_id = -1;
    uint64_t account = 0;
    std::string type;
    if (!activator.SlotUser(slot, user_id, account, type))
      continue;
    char name[100] = {};
    sceUserServiceGetUserName(user_id, name, sizeof(name));
    char b64[16] = {};
    char dec[24] = {};
    if (account) {
      base64_encode(account, b64, sizeof(b64));
      snprintf(dec, sizeof(dec), "%llu", static_cast<unsigned long long>(account));
    }
    cJSON *u = cJSON_CreateObject();
    cJSON_AddNumberToObject(u, "slot", slot);
    cJSON_AddNumberToObject(u, "userId", user_id);
    cJSON_AddStringToObject(u, "name", name);
    cJSON_AddBoolToObject(u, "activated", account != 0);
    cJSON_AddStringToObject(u, "accountId", b64);
    cJSON_AddStringToObject(u, "accountIdDecimal", dec);
    cJSON_AddStringToObject(u, "type", type.c_str());
    cJSON_AddBoolToObject(u, "foreground", user_id == fg_user);
    cJSON_AddItemToArray(users, u);
    if (user_id == fg_user) {
      cJSON_AddStringToObject(root, "accountId", b64);
      cJSON_AddStringToObject(root, "accountIdDecimal", dec);
    }
  }
  char pin[12] = "";
  if (g_web_pin)
    snprintf(pin, sizeof(pin), "%04u%04u", g_web_pin / 10000, g_web_pin % 10000);
  cJSON_AddStringToObject(root, "pin", pin);
  cJSON_AddNumberToObject(root, "time", static_cast<double>(g_web_pin_time));
  cJSON_AddNumberToObject(root, "pinSeconds", kPinSeconds);
  cJSON_AddStringToObject(root, "state",
                          state_name(g_pairing_state.load(std::memory_order_acquire)));
  cJSON_AddStringToObject(root, "lastOp", g_last_op.c_str());
  cJSON_AddBoolToObject(root, "lastOk", g_last_ok);
  cJSON_AddStringToObject(root, "lastError", g_last_error.c_str());
  cJSON_AddNumberToObject(root, "updated", static_cast<double>(time(nullptr)));
  char *text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!text)
    return;
  const std::string tmp = std::string(kRpStatus) + ".tmp";
  if (FILE *f = fopen(tmp.c_str(), "wb")) {
    fputs(text, f);
    fclose(f);
    rename(tmp.c_str(), kRpStatus);
  }
  cJSON_free(text);
}

void web_request(const std::string &op, int slot) {
  g_last_op = op;
  g_last_ok = false;
  g_last_error.clear();
  if (op == "pin") {
    static bool initialized = false;
    Activator activator(true);
    if (!activator.Valid()) {
      g_last_error = "account";
    } else if (activator.IsNotActivated()) {
      g_last_error = "not_activated";
    } else {
      if (!initialized) {
        initialize_remote_play();
        initialized = true;
      }
      const uint32_t pin = generate_pin_code();
      if (pin) {
        g_web_pin = pin;
        g_web_pin_time = time(nullptr);
        g_last_ok = true;
      } else {
        g_last_error = "pin";
      }
    }
  } else if (op == "cancel") {
    if (remote_play::try_finish_pairing(g_pairing_state,
                                        remote_play::PairingState::Cancelled))
      invalidate_pin_registration("web_cancel");
    g_web_pin = 0;
    g_last_ok = true;
  } else if (op == "activate") {
    Activator activator(true);
    int32_t user_id = -1;
    uint64_t account = 0;
    std::string type;
    char name[100] = {};
    if (activator.SlotUser(slot, user_id, account, type))
      sceUserServiceGetUserName(user_id, name, sizeof(name));
    g_last_ok = name[0] && activator.ActivateSlot(slot, name);
    if (!g_last_ok)
      g_last_error = "activate";
    else
      onion_notify_debug(/*show_watermark=*/0, "notify.remote_play.activated_fmt", name);
  } else {
    g_last_ok = true; /* "list": the status below is the answer */
  }
  LOG_INFO("remote play from NeoWEB: %s slot=%d ok=%d %s", op.c_str(), slot,
           g_last_ok ? 1 : 0, g_last_error.c_str());
}

void *web_thread(void *) {
  remote_play::PairingState last = remote_play::PairingState::Idle;
  sleep(5); /* let the menu settle, then say who is there */
  write_status();
  for (;;) {
    sleep(1);
    std::string text;
    if (FILE *f = fopen(kRpRequest, "rb")) {
      char buf[1024];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < 4096)
        text.append(buf, n);
      fclose(f);
      unlink(kRpRequest);
    }
    bool changed = false;
    if (!text.empty()) {
      onion_cjson::Root req(text);
      web_request(req ? onion_cjson::string_item(req.get(), "op", "list") : "list",
                  req ? onion_cjson::int_item(req.get(), "slot", 0) : 0);
      changed = true;
    }
    const remote_play::PairingState now = g_pairing_state.load(std::memory_order_acquire);
    if (now == remote_play::PairingState::Waiting && g_web_pin &&
        time(nullptr) - g_web_pin_time > kPinSeconds &&
        remote_play::try_finish_pairing(g_pairing_state,
                                        remote_play::PairingState::Cancelled)) {
      invalidate_pin_registration("web_pin_expired");
      g_web_pin = 0;
    }
    if (now != last) {
      if (now != remote_play::PairingState::Waiting)
        g_web_pin = 0;
      last = now;
      changed = true;
    }
    if (changed)
      write_status();
  }
  return nullptr;
}
} // namespace

void remote_play_web_note_pin(uint32_t pin) {
  g_web_pin = pin;
  g_web_pin_time = time(nullptr);
  write_status();
}

void remote_play_web_start() {
  static bool started = false;
  if (started)
    return;
  started = true;
  pthread_t thread;
  if (pthread_create(&thread, nullptr, web_thread, nullptr) == 0)
    pthread_detach(thread);
}
