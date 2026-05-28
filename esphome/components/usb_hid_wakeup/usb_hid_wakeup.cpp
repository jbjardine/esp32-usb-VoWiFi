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

#include <tinyusb.h>
#include <tinyusb_default_config.h>
#include <class/hid/hid_device.h>

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

static volatile bool s_suspended = false;

extern "C" {

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  (void) instance;
  return hid_report_descriptor;
}

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

// ----- Low-level keyboard send ----------------------------------------------

void UsbHidWakeupComponent::send_key_(uint8_t modifier, uint8_t keycode) {
  if (!tud_mounted())
    return;
  uint8_t kc[6] = {keycode, 0, 0, 0, 0, 0};
  tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, kc);
}

void UsbHidWakeupComponent::release_keys_() {
  if (!tud_mounted())
    return;
  tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL);
}

uint32_t UsbHidWakeupComponent::type_string_(const std::string &text, uint32_t start_delay_ms) {
  const uint32_t key_ms = 22;  // press duration
  const uint32_t gap_ms = 16;  // release-to-next gap
  uint32_t t = start_delay_ms;
  for (char c : text) {
    uint8_t keycode;
    bool shift;
    if (!this->char_to_keycode_(c, keycode, shift)) {
      ESP_LOGW(TAG, "no keycode for char 0x%02X in current layout — skipped", (uint8_t) c);
      continue;
    }
    uint8_t mod = shift ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
    this->set_timeout("", t, [this, mod, keycode]() { this->send_key_(mod, keycode); });
    this->set_timeout("", t + key_ms, [this]() { this->release_keys_(); });
    t += key_ms + gap_ms;
  }
  return t;
}

void UsbHidWakeupComponent::send_system_power_down_() {
  if (!tud_mounted())
    return;
  uint8_t down = 1;  // 2-bit field: 1=System Power Down, 2=Sleep, 3=Wake
  tud_hid_report(REPORT_ID_SYSCTRL, &down, sizeof(down));
  this->set_timeout("", 60, [this]() {
    if (!tud_mounted())
      return;
    uint8_t release = 0;
    tud_hid_report(REPORT_ID_SYSCTRL, &release, sizeof(release));
  });
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

  bool mounted = tud_mounted();
  if (mounted != this->last_mounted_) {
    this->last_mounted_ = mounted;
    ESP_LOGD(TAG, "USB host %s", mounted ? "connected" : "disconnected");
    for (auto *bs : this->mounted_sensors_)
      bs->publish_state(mounted);
  }

  bool suspended = s_suspended;
  if (suspended != this->last_suspended_) {
    this->last_suspended_ = suspended;
    ESP_LOGD(TAG, "USB bus %s (host %s)", suspended ? "suspended" : "resumed",
             suspended ? "asleep" : "awake");
    for (auto *bs : this->suspended_sensors_)
      bs->publish_state(suspended);
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

void UsbHidWakeupComponent::request_type_test() {
  if (!this->tinyusb_started_ || !tud_mounted()) {
    ESP_LOGW(TAG, "type test requested but USB not mounted — ignoring");
    return;
  }
  // SAFE: types the command string into whatever window is focused (open
  // Notepad first), with NO Win+R, NO Enter, NO power down. Use it to verify
  // the keyboard_layout produces the exact text before trusting force_shutdown.
  ESP_LOGI(TAG, "type test: typing 'shutdown /s /f /t 0' into focused window (no execution)");
  this->type_string_("shutdown /s /f /t 0", 100);
}

void UsbHidWakeupComponent::request_force_shutdown() {
  if (!this->tinyusb_started_ || !tud_mounted()) {
    ESP_LOGW(TAG, "force shutdown requested but USB not mounted — ignoring");
    return;
  }
  ESP_LOGW(TAG, "FORCE SHUTDOWN sequence initiated (Windows)");

  // Leg 1 (unlocked session): Win+R, then type the forced-shutdown command.
  this->set_timeout("", 0, [this]() { this->send_key_(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_R); });
  this->set_timeout("", 60, [this]() { this->release_keys_(); });

  // Wait for the Run dialog to appear, then type the command.
  uint32_t after_type = this->type_string_("shutdown /s /f /t 0", 600);

  // Enter to execute.
  this->set_timeout("", after_type + 80, [this]() { this->send_key_(0, HID_KEY_ENTER); });
  this->set_timeout("", after_type + 130, [this]() { this->release_keys_(); });

  // Leg 2 (locked session fallback): ACPI System Power Down. Harmless if the
  // machine is already shutting down from leg 1.
  this->set_timeout("", after_type + 1200, [this]() { this->send_system_power_down_(); });
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
  if (this->is_failed())
    ESP_LOGE(TAG, "  Component FAILED to install TinyUSB driver");
}

}  // namespace usb_hid_wakeup
}  // namespace esphome
