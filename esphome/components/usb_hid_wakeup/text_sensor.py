"""Text sensor platform for usb_hid_wakeup.

A single diagnostic status entity reflecting the host power state in one of:
  - "Awake"          host powered and not asleep (usable now)
  - "Asleep or off"  USB bus suspended (USB can't tell sleep from shutdown)
  - "Disconnected"   no USB host (unplugged / no standby power)

Pair with `entity_category: diagnostic` to file it under the device's
Diagnostic section in Home Assistant.
"""
from esphome import codegen as cg
from esphome.components import text_sensor

from . import UsbHidWakeupComponent, usb_hid_wakeup_ns

DEPENDENCIES = ["usb_hid_wakeup"]

import esphome.config_validation as cv

CONF_USB_HID_WAKEUP_ID = "usb_hid_wakeup_id"

UsbHidWakeupStatusTextSensor = usb_hid_wakeup_ns.class_(
    "UsbHidWakeupStatusTextSensor", text_sensor.TextSensor
)

CONFIG_SCHEMA = text_sensor.text_sensor_schema(UsbHidWakeupStatusTextSensor).extend(
    {cv.GenerateID(CONF_USB_HID_WAKEUP_ID): cv.use_id(UsbHidWakeupComponent)}
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    parent = await cg.get_variable(config[CONF_USB_HID_WAKEUP_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.register_status_text_sensor(var))
