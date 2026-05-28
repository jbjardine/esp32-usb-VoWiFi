#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/button/button.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

#include <tinyusb.h>  // tusb_desc_device_t

#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace usb_hid_wakeup {

enum KeyboardLayout : uint8_t {
  LAYOUT_AZERTY = 0,
  LAYOUT_US = 1,
};

enum ButtonAction : uint8_t {
  ACTION_WAKEUP = 0,
  ACTION_FORCE_SHUTDOWN = 1,
  ACTION_TYPE_TEST = 2,
};

class UsbHidWakeupComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Button actions
  void request_wakeup();
  void request_force_shutdown();
  void request_type_test();  // types the shutdown command into the focused window, no execution

  // Sensor registration
  void register_mounted_sensor(binary_sensor::BinarySensor *bs) { this->mounted_sensors_.push_back(bs); }
  void register_suspended_sensor(binary_sensor::BinarySensor *bs) { this->suspended_sensors_.push_back(bs); }

  // USB identity config (Phase A) — defaults match the native firmware
  void set_usb_vid(uint16_t v) { this->vid_ = v; }
  void set_usb_pid(uint16_t v) { this->pid_ = v; }
  void set_manufacturer(const std::string &s) { this->manufacturer_ = s; }
  void set_product(const std::string &s) { this->product_ = s; }
  void set_serial(const std::string &s) { this->serial_ = s; }
  void set_keyboard_layout(KeyboardLayout l) { this->layout_ = l; }

 protected:
  // Keyboard typing engine (Phase C)
  void send_key_(uint8_t modifier, uint8_t keycode);
  void release_keys_();
  bool char_to_keycode_(char c, uint8_t &keycode, bool &shift) const;
  // Returns the time offset (ms) just after the last scheduled keystroke.
  uint32_t type_string_(const std::string &text, uint32_t start_delay_ms);
  void send_system_power_down_();

  std::vector<binary_sensor::BinarySensor *> mounted_sensors_;
  std::vector<binary_sensor::BinarySensor *> suspended_sensors_;
  bool last_mounted_{false};
  bool last_suspended_{false};
  bool tinyusb_started_{false};

  // Phase A — configurable USB identity (defaults = native firmware values)
  uint16_t vid_{0x303A};
  uint16_t pid_{0x4004};
  std::string manufacturer_{"ESP"};
  std::string product_{"Wakeup Keyboard Device"};
  std::string serial_{"123456"};
  KeyboardLayout layout_{LAYOUT_AZERTY};

  // Backing storage handed to TinyUSB at install time — must outlive setup().
  tusb_desc_device_t dev_desc_{};
  const char *strings_[5]{};
};

class UsbHidWakeupButton : public button::Button, public Parented<UsbHidWakeupComponent> {
 public:
  void set_action(ButtonAction a) { this->action_ = a; }

 protected:
  void press_action() override {
    switch (this->action_) {
      case ACTION_FORCE_SHUTDOWN:
        this->parent_->request_force_shutdown();
        break;
      case ACTION_TYPE_TEST:
        this->parent_->request_type_test();
        break;
      case ACTION_WAKEUP:
      default:
        this->parent_->request_wakeup();
        break;
    }
  }
  ButtonAction action_{ACTION_WAKEUP};
};

class UsbHidWakeupMountedSensor : public binary_sensor::BinarySensor, public Parented<UsbHidWakeupComponent> {};
class UsbHidWakeupSuspendedSensor : public binary_sensor::BinarySensor, public Parented<UsbHidWakeupComponent> {};

}  // namespace usb_hid_wakeup
}  // namespace esphome
