// USB HID keyboard + remote-wakeup for ESPHome on ESP32-S3.
//
// Descriptors, string table and HID callbacks are ported byte-for-byte from
// the native ESP-IDF reference firmware at master/main/usb.c:14-122. The only
// structural difference: ESPHome's component lifecycle replaces the native
// FreeRTOS event-group + usb_task. esp_tinyusb spawns its own task (via
// TINYUSB_TASK_DEFAULT) that pumps tud_task(), so our loop() only polls the
// mount state for the binary_sensor — it does not drive the USB stack.

#include "usb_hid_wakeup.h"
#include "esphome/core/log.h"

#include <tinyusb.h>
#include <tinyusb_default_config.h>
#include <class/hid/hid_device.h>

namespace esphome {
namespace usb_hid_wakeup {

static const char *const TAG = "usb_hid_wakeup";

// ----- USB descriptors (ported from master/main/usb.c:16-39) ----------------

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)

static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_ITF_PROTOCOL_KEYBOARD))};

static const char *hid_string_descriptor[5] = {
    (char[]){0x09, 0x04},       // 0: supported language = English (0x0409)
    "ESP",                      // 1: Manufacturer
    "Wakeup Keyboard Device",   // 2: Product
    "123456",                   // 3: Serial
    "Wakeup HID interface",     // 4: HID interface
};

static const uint8_t hid_configuration_descriptor[] = {
    // Config number, interface count, string index, total length,
    // attribute (REMOTE_WAKEUP bit set — required for tud_remote_wakeup),
    // power in mA.
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    // Interface number, string index, boot protocol, report descriptor len,
    // EP In address, size & polling interval.
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(hid_report_descriptor), 0x81, 16, 10),
};

// ----- TinyUSB callbacks (C linkage — TinyUSB calls these by symbol) ---------

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
  return 0;  // returning 0 STALLs the request — we are output-only
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
  (void) instance;
  (void) report_id;
  (void) report_type;
  (void) buffer;
  (void) bufsize;
}

}  // extern "C"

// ----- Component lifecycle ---------------------------------------------------

void UsbHidWakeupComponent::setup() {
  ESP_LOGCONFIG(TAG, "Installing TinyUSB HID keyboard driver...");

  const tinyusb_config_t tusb_cfg = {
      .task = TINYUSB_TASK_DEFAULT(),
      .descriptor =
          {
              .device = nullptr,  // use esp_tinyusb default device descriptor (Espressif VID/PID)
              .string = hid_string_descriptor,
              .string_count = sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]),
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

  // Poll mount state and publish to registered binary sensors on change.
  // tud_mounted() is a cheap bool read; esp_tinyusb's own task keeps it fresh.
  bool mounted = tud_mounted();
  if (mounted != this->last_mounted_) {
    this->last_mounted_ = mounted;
    ESP_LOGD(TAG, "USB host %s", mounted ? "connected" : "disconnected");
    for (auto *bs : this->mounted_sensors_)
      bs->publish_state(mounted);
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
  // tud_remote_wakeup() only signals if the bus is suspended (host asleep) AND
  // the host enabled remote wakeup. It returns false otherwise — that's fine:
  // a press while the PC is already awake is simply a no-op.
  bool sent = tud_remote_wakeup();
  if (sent) {
    ESP_LOGI(TAG, "remote wakeup signal sent to host");
  } else {
    ESP_LOGI(TAG, "remote wakeup not sent (host not suspended or wakeup not enabled)");
  }
}

void UsbHidWakeupComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "USB HID Wakeup:");
  ESP_LOGCONFIG(TAG, "  Driver installed: %s", YESNO(this->tinyusb_started_));
  ESP_LOGCONFIG(TAG, "  Product string:   %s", hid_string_descriptor[2]);
  ESP_LOGCONFIG(TAG, "  Manufacturer:     %s", hid_string_descriptor[1]);
  ESP_LOGCONFIG(TAG, "  Remote wakeup:    enabled (bmAttributes bit 5 set)");
  ESP_LOGCONFIG(TAG, "  Mounted sensors:  %u", (unsigned) this->mounted_sensors_.size());
  if (this->is_failed())
    ESP_LOGE(TAG, "  Component FAILED to install TinyUSB driver");
}

}  // namespace usb_hid_wakeup
}  // namespace esphome
