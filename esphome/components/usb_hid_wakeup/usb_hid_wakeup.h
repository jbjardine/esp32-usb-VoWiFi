#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/button/button.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

#include <vector>

namespace esphome {
namespace usb_hid_wakeup {

class UsbHidWakeupMountedSensor;

class UsbHidWakeupComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void request_wakeup();
  void register_mounted_sensor(binary_sensor::BinarySensor *bs) { this->mounted_sensors_.push_back(bs); }

 protected:
  std::vector<binary_sensor::BinarySensor *> mounted_sensors_;
  bool last_mounted_{false};
  bool tinyusb_started_{false};
};

class UsbHidWakeupButton : public button::Button, public Parented<UsbHidWakeupComponent> {
 protected:
  void press_action() override { this->parent_->request_wakeup(); }
};

class UsbHidWakeupMountedSensor : public binary_sensor::BinarySensor, public Parented<UsbHidWakeupComponent> {};

}  // namespace usb_hid_wakeup
}  // namespace esphome
