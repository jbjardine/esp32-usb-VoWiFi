// USB HID keyboard + remote-wakeup + Windows force-shutdown for ESPHome (S3).
//
// Base descriptors/callbacks ported from master/main/usb.c. v0.2 adds:
//  - configurable USB identity (VID/PID/strings) via a custom device descriptor
//  - a System Control report (report ID 2) alongside the keyboard (report ID 1)
//  - a non-blocking keystroke engine with an AZERTY/US layout map
//  - request_force_shutdown(): Win+R -> "shutdown /s /f /t 0" -> Enter (forced
//    when the session is unlocked) followed by an ACPI System Power Down
//    (works when locked, but graceful). Target OS: Windows.
//  - tud_suspend/resume callbacks feeding a `suspended` binary sensor.

#include "usb_hid_wakeup.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"  // millis()

#include <tinyusb.h>
#include <tinyusb_default_config.h>
#include <class/hid/hid_device.h>

#include <atomic>

namespace esphome {
namespace usb_hid_wakeup {

static const char *const TAG = "usb_hid_wakeup";

enum : uint8_t {
  REPORT_ID_KEYBOARD = 1,
  REPORT_ID_SYSCTRL = 2,
};

// ----- USB descriptors ------------------------------------------------------

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)

// Multi-report: keyboard (ID 1) + system control (ID 2). The system control
// report carries the "System Power Down" usage used for the locked-screen leg.
static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_SYSTEM_CONTROL(HID_REPORT_ID(REPORT_ID_SYSCTRL)),
};

// USB 2.0 language descriptor (English 0x0409). Index 0 of the string table.
static const char langid_descriptor[] = {0x09, 0x04};

static const uint8_t hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(hid_report_descriptor), 0x81, 16, 10),
};

// ----- TinyUSB C-linkage callbacks ------------------------------------------

// Written from the TinyUSB task (core 1) and read from loop() (core 0):
// std::atomic gives the cross-core visibility `volatile` does not guarantee.
static std::atomic<bool> s_suspended{false};

extern "C" {

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  (void) instance;
  return hid_report_descriptor;
}

// tud_hid_get_report_cb / tud_hid_set_report_cb are MANDATORY in TinyUSB (not
// weak) — the HID class links against them. Keep these stubs even though we are
// output-only; deleting them breaks the build.
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
  (void) instance;
  (void) report_id;
  (void) report_type;
  (void) buffer;
  (void) reqlen;
  return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
  (void) instance;
  (void) report_id;
  (void) report_type;
  (void) buffer;
  (void) bufsize;
}

// Bus suspend/resume — overrides esp_tinyusb's weak defaults. Lets us expose
// a `suspended` binary sensor (true == host asleep). Purely passive.
void tud_suspend_cb(bool remote_wakeup_en) {
  (void) remote_wakeup_en;
  s_suspended = true;
}
void tud_resume_cb(void) { s_suspended = false; }

}  // extern "C"

// ----- Keyboard layout map --------------------------------------------------
// HID keycodes are positional. On a French AZERTY Windows host, the same
// physical key produces different characters than US — so the keycode we send
// to type 'w', '/', a digit, etc. differs. We map only what shutdown commands
// need (letters, space, digits, '/', '-').

bool UsbHidWakeupComponent::char_to_keycode_(char c, uint8_t &keycode, bool &shift) const {
  shift = false;
  if (this->layout_ == LAYOUT_AZERTY) {
    if (c >= 'a' && c <= 'z') {
      switch (c) {
        case 'a': keycode = HID_KEY_Q; return true;     // A<->Q swap
        case 'q': keycode = HID_KEY_A; return true;
        case 'z': keycode = HID_KEY_W; return true;     // Z<->W swap
        case 'w': keycode = HID_KEY_Z; return true;
        case 'm': keycode = HID_KEY_SEMICOLON; return true;  // M moved to ';' pos
        default: keycode = HID_KEY_A + (c - 'a'); return true;
      }
    }
    if (c >= '1' && c <= '9') {  // digits need Shift on AZERTY
      keycode = HID_KEY_1 + (c - '1');
      shift = true;
      return true;
    }
    switch (c) {
      case '0': keycode = HID_KEY_0; shift = true; return true;
      case ' ': keycode = HID_KEY_SPACE; return true;
      case '/': keycode = HID_KEY_PERIOD; shift = true; return true;  // '/' = Shift+':' key
      case '-': keycode = HID_KEY_6; return true;                     // '-' unshifted on '6' key
      case '.': keycode = HID_KEY_COMMA; shift = true; return true;   // '.' = Shift+';' key
      default: return false;
    }
  }
  // LAYOUT_US
  if (c >= 'a' && c <= 'z') {
    keycode = HID_KEY_A + (c - 'a');
    return true;
  }
  if (c >= '1' && c <= '9') {
    keycode = HID_KEY_1 + (c - '1');
    return true;
  }
  switch (c) {
    case '0': keycode = HID_KEY_0; return true;
    case ' ': keycode = HID_KEY_SPACE; return true;
    case '/': keycode = HID_KEY_SLASH; return true;
    case '-': keycode = HID_KEY_MINUS; return true;
    case '.': keycode = HID_KEY_PERIOD; return true;
    default: return false;
  }
}

// ----- Event queue (typing engine) ------------------------------------------

void UsbHidWakeupComponent::begin_sequence_() {
  // Starting a new sequence drops any in-flight one (double-tap / HA retry).
  // Enqueue a leading release-all so a key/modifier left held by the discarded
  // sequence (e.g. Win+R whose release was dropped) is cleared before we type.
  this->events_.clear();
  this->event_cursor_ = 0;
  this->seq_base_ = millis();
  this->enqueue_key_(0, 0, 0);  // release-all guard
}

void UsbHidWakeupComponent::enqueue_key_(uint32_t offset_ms, uint8_t modifier, uint8_t keycode) {
  this->events_.push_back(UsbEvent{this->seq_base_ + offset_ms, EVT_KBD, modifier, keycode, 0});
}

void UsbHidWakeupComponent::enqueue_sysctrl_(uint32_t offset_ms, uint8_t payload) {
  this->events_.push_back(UsbEvent{this->seq_base_ + offset_ms, EVT_SYSCTRL, 0, 0, payload});
}

uint32_t UsbHidWakeupComponent::type_string_(const std::string &text, uint32_t start_offset_ms) {
  const uint32_t key_hold_ms = 40;   // press persists this long before release
  const uint32_t inter_key_ms = 25;  // gap between a release and the next press
  uint32_t t = start_offset_ms;
  for (char c : text) {
    uint8_t keycode;
    bool shift;
    if (!this->char_to_keycode_(c, keycode, shift)) {
      ESP_LOGW(TAG, "no keycode for char 0x%02X in current layout — skipped", (uint8_t) c);
      continue;
    }
    uint8_t mod = shift ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
    this->enqueue_key_(t, mod, keycode);            // press
    this->enqueue_key_(t + key_hold_ms, 0, 0);      // release all
    t += key_hold_ms + inter_key_ms;
  }
  return t;
}

void UsbHidWakeupComponent::process_events_() {
  if (this->event_cursor_ >= this->events_.size())
    return;

  const uint32_t now = millis();
  while (this->event_cursor_ < this->events_.size()) {
    UsbEvent &e = this->events_[this->event_cursor_];
    if ((int32_t) (now - e.due) < 0)
      break;  // not due yet
    if (!tud_mounted()) {
      ESP_LOGW(TAG, "host vanished mid-sequence — aborting");
      this->events_.clear();
      this->event_cursor_ = 0;
      return;
    }
    if (!tud_hid_ready())
      break;  // endpoint busy — retry next loop (prevents dropped reports)

    if (e.kind == EVT_KBD) {
      if (e.keycode == 0) {
        tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL);  // release all
      } else {
        uint8_t kc[6] = {e.keycode, 0, 0, 0, 0, 0};
        tud_hid_keyboard_report(REPORT_ID_KEYBOARD, e.modifier, kc);
      }
    } else {  // EVT_SYSCTRL
      tud_hid_report(REPORT_ID_SYSCTRL, &e.payload, 1);
    }
    this->event_cursor_++;
    // tud_hid_ready() is now false (EP busy) so the next iteration breaks —
    // effectively one report per loop when events are back-to-back.
  }

  if (this->event_cursor_ >= this->events_.size()) {
    this->events_.clear();
    this->event_cursor_ = 0;
  }
}

// ----- Component lifecycle --------------------------------------------------

void UsbHidWakeupComponent::setup() {
  ESP_LOGCONFIG(TAG, "Installing TinyUSB HID keyboard driver...");

  // Build the configurable device descriptor (Phase A).
  this->dev_desc_.bLength = sizeof(tusb_desc_device_t);
  this->dev_desc_.bDescriptorType = TUSB_DESC_DEVICE;
  this->dev_desc_.bcdUSB = 0x0200;
  this->dev_desc_.bDeviceClass = 0x00;
  this->dev_desc_.bDeviceSubClass = 0x00;
  this->dev_desc_.bDeviceProtocol = 0x00;
  this->dev_desc_.bMaxPacketSize0 = 64;
  this->dev_desc_.idVendor = this->vid_;
  this->dev_desc_.idProduct = this->pid_;
  this->dev_desc_.bcdDevice = 0x0100;
  this->dev_desc_.iManufacturer = 0x01;
  this->dev_desc_.iProduct = 0x02;
  this->dev_desc_.iSerialNumber = 0x03;
  this->dev_desc_.bNumConfigurations = 0x01;

  this->strings_[0] = langid_descriptor;
  this->strings_[1] = this->manufacturer_.c_str();
  this->strings_[2] = this->product_.c_str();
  this->strings_[3] = this->serial_.c_str();
  this->strings_[4] = "Wakeup HID interface";

  const tinyusb_config_t tusb_cfg = {
      .task = TINYUSB_TASK_DEFAULT(),
      .descriptor =
          {
              .device = &this->dev_desc_,
              .string = this->strings_,
              .string_count = 5,
              .full_speed_config = hid_configuration_descriptor,
          },
  };

  esp_err_t err = tinyusb_driver_install(&tusb_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  this->tinyusb_started_ = true;
  ESP_LOGCONFIG(TAG, "TinyUSB HID driver installed");
}

void UsbHidWakeupComponent::loop() {
  if (!this->tinyusb_started_)
    return;

  this->process_events_();

  bool mounted = tud_mounted();
  bool suspended = s_suspended;
  bool awake = mounted && !suspended;  // host powered AND not asleep == usable now
  // 3-state diagnostic string matching observed reality on the host:
  //   enumerated + active  -> "Allumé"  (PC on)
  //   enumerated + suspended -> "Veille" (PC in S3 sleep, bus suspended)
  //   not enumerated        -> "Éteint" (PC off in S5 de-enumerates; also cable unplugged)
  const char *status = !mounted ? "Éteint" : (suspended ? "Veille" : "Allumé");

  // First loop: publish current state so HA shows on/off instead of "unknown"
  // (change-detection alone never fires for a state that hasn't transitioned).
  if (!this->initial_published_) {
    this->initial_published_ = true;
    this->last_mounted_ = mounted;
    this->last_suspended_ = suspended;
    this->last_awake_ = awake;
    this->last_status_ = status;
    for (auto *bs : this->mounted_sensors_)
      bs->publish_state(mounted);
    for (auto *bs : this->suspended_sensors_)
      bs->publish_state(suspended);
    for (auto *bs : this->awake_sensors_)
      bs->publish_state(awake);
    for (auto *ts : this->status_text_sensors_)
      ts->publish_state(status);
    return;
  }

  if (mounted != this->last_mounted_) {
    this->last_mounted_ = mounted;
    ESP_LOGD(TAG, "USB host %s", mounted ? "connected" : "disconnected");
    for (auto *bs : this->mounted_sensors_)
      bs->publish_state(mounted);
  }

  if (suspended != this->last_suspended_) {
    this->last_suspended_ = suspended;
    ESP_LOGD(TAG, "USB bus %s (host %s)", suspended ? "suspended" : "resumed",
             suspended ? "asleep" : "awake");
    for (auto *bs : this->suspended_sensors_)
      bs->publish_state(suspended);
  }

  if (awake != this->last_awake_) {
    this->last_awake_ = awake;
    ESP_LOGD(TAG, "PC %s", awake ? "awake/on" : "asleep-or-off");
    for (auto *bs : this->awake_sensors_)
      bs->publish_state(awake);
  }

  if (status != this->last_status_) {  // pointer compare OK: fixed string literals
    this->last_status_ = status;
    for (auto *ts : this->status_text_sensors_)
      ts->publish_state(status);
  }
}

void UsbHidWakeupComponent::request_wakeup() {
  if (!this->tinyusb_started_) {
    ESP_LOGW(TAG, "wakeup requested but TinyUSB not started");
    return;
  }
  if (!tud_mounted()) {
    ESP_LOGW(TAG, "wakeup requested but USB not mounted to a host — ignoring");
    return;
  }
  bool sent = tud_remote_wakeup();
  ESP_LOGI(TAG, "remote wakeup %s", sent ? "signal sent" : "not sent (host not suspended)");
}

bool UsbHidWakeupComponent::ready_for_sequence_(const char *what) {
  if (!this->tinyusb_started_ || !tud_mounted()) {
    ESP_LOGW(TAG, "%s requested but USB not mounted to a host — ignoring", what);
    return false;
  }
  this->begin_sequence_();
  return true;
}

void UsbHidWakeupComponent::request_type_test() {
  // SAFE: types the command string into whatever window is focused (open
  // Notepad first), with NO Win+R, NO Enter, NO power down. Use it to verify
  // the keyboard_layout produces the exact text before trusting force_shutdown.
  if (!this->ready_for_sequence_("type test"))
    return;
  ESP_LOGI(TAG, "type test: typing 'shutdown /s /f /t 0' into focused window (no execution)");
  this->type_string_("shutdown /s /f /t 0", 100);
}

uint32_t UsbHidWakeupComponent::enqueue_force_macro_(uint32_t start_offset_ms) {
  // Win+R -> wait for Run dialog -> type "shutdown /s /f /t 0" -> Enter.
  this->enqueue_key_(start_offset_ms, KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_R);
  this->enqueue_key_(start_offset_ms + 60, 0, 0);
  uint32_t after_type = this->type_string_("shutdown /s /f /t 0", start_offset_ms + 600);
  this->enqueue_key_(after_type + 80, 0, HID_KEY_ENTER);
  this->enqueue_key_(after_type + 130, 0, 0);
  return after_type + 130;
}

void UsbHidWakeupComponent::enqueue_acpi_powerdown_(uint32_t offset_ms) {
  this->enqueue_sysctrl_(offset_ms, 1);       // 1 = System Power Down
  this->enqueue_sysctrl_(offset_ms + 60, 0);  // release
}

void UsbHidWakeupComponent::request_force_shutdown() {
  if (!this->ready_for_sequence_("force shutdown"))
    return;
  ESP_LOGW(TAG, "FORCE shutdown (type /f command — unlocked session)");
  this->enqueue_force_macro_(0);
}

void UsbHidWakeupComponent::request_acpi_shutdown() {
  if (!this->ready_for_sequence_("ACPI shutdown"))
    return;
  ESP_LOGW(TAG, "ACPI System Power Down (locked-safe, graceful)");
  this->enqueue_acpi_powerdown_(0);
}

void UsbHidWakeupComponent::request_auto_shutdown() {
  if (!this->ready_for_sequence_("auto shutdown"))
    return;
  ESP_LOGW(TAG, "AUTO shutdown (force macro + ACPI fallback)");
  uint32_t after = this->enqueue_force_macro_(0);
  this->enqueue_acpi_powerdown_(after + 1100);
}

void UsbHidWakeupComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "USB HID Wakeup:");
  ESP_LOGCONFIG(TAG, "  Driver installed: %s", YESNO(this->tinyusb_started_));
  ESP_LOGCONFIG(TAG, "  VID:PID:          %04X:%04X", this->vid_, this->pid_);
  ESP_LOGCONFIG(TAG, "  Product:          %s", this->product_.c_str());
  ESP_LOGCONFIG(TAG, "  Manufacturer:     %s", this->manufacturer_.c_str());
  ESP_LOGCONFIG(TAG, "  Keyboard layout:  %s", this->layout_ == LAYOUT_AZERTY ? "AZERTY" : "US");
  ESP_LOGCONFIG(TAG, "  Remote wakeup:    enabled (bmAttributes bit 5)");
  ESP_LOGCONFIG(TAG, "  Mounted sensors:  %u", (unsigned) this->mounted_sensors_.size());
  ESP_LOGCONFIG(TAG, "  Suspended sensors:%u", (unsigned) this->suspended_sensors_.size());
  ESP_LOGCONFIG(TAG, "  Awake sensors:    %u", (unsigned) this->awake_sensors_.size());
  ESP_LOGCONFIG(TAG, "  Status text:      %u", (unsigned) this->status_text_sensors_.size());
  if (this->is_failed())
    ESP_LOGE(TAG, "  Component FAILED to install TinyUSB driver");
}

}  // namespace usb_hid_wakeup
}  // namespace esphome
