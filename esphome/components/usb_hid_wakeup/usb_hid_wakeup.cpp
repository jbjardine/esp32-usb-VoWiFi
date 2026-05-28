// Phase 1 scaffolding: skeleton only — no TinyUSB calls yet.
//
// Phase 2 will port the HID descriptor + tud_remote_wakeup() logic from the
// reference implementation at master/main/usb.c:14-103.
//
// Open blockers documented in the plan:
//   1. Configuration descriptor bmAttributes Remote Wakeup bit (0x20) must
//      be set — ESPHome's tinyusb component doesn't expose this via YAML.
//      Workaround plan: override tud_descriptor_configuration_cb() directly.
//   2. tud_remote_wakeup() only works when tud_suspended() == true.
//      Workaround plan: log warn and bail if not suspended.

#include "usb_hid_wakeup.h"
#include "esphome/core/log.h"

namespace esphome {
namespace usb_hid_wakeup {

static const char *const TAG = "usb_hid_wakeup";

void UsbHidWakeupComponent::setup() {
  ESP_LOGI(TAG, "setup() — Phase 1 scaffolding, TinyUSB not yet initialized");
  // TODO Phase 2:
  //   - Copy hid_report_descriptor[] from master/main/usb.c:18-20
  //   - Copy hid_configuration_descriptor[] from master/main/usb.c:33-39
  //     (note: TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP must be set in bmAttributes)
  //   - Copy hid_string_descriptor[] from master/main/usb.c:22-31
  //   - Call tinyusb_driver_install() with the above
  //   - Override tud_descriptor_configuration_cb() in this TU
}

void UsbHidWakeupComponent::loop() {
  // TODO Phase 3:
  //   - Read tud_mounted() each tick
  //   - On state change, publish_state(mounted) to each sensor in mounted_sensors_
  //   - Throttle to avoid spamming (debounce ~100ms)
}

void UsbHidWakeupComponent::request_wakeup() {
  ESP_LOGW(TAG, "request_wakeup() — Phase 1 scaffolding, no-op until Phase 2");
  // TODO Phase 2:
  //   if (!tud_mounted()) { ESP_LOGW(TAG, "USB not mounted"); return; }
  //   if (!tud_suspended()) { ESP_LOGW(TAG, "USB host not suspended, ignoring"); return; }
  //   tud_remote_wakeup();
}

void UsbHidWakeupComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "USB HID Wakeup:");
  ESP_LOGCONFIG(TAG, "  Mounted sensors registered: %u", (unsigned) this->mounted_sensors_.size());
  ESP_LOGCONFIG(TAG, "  Phase: 1 (scaffolding — no USB activity)");
}

}  // namespace usb_hid_wakeup
}  // namespace esphome
