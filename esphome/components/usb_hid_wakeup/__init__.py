"""USB HID Wakeup external component for ESPHome.

Reproduces the minimal behavior of the native ESP-IDF firmware on the
`master` branch (https://github.com/jbjardine/esp32-usb-VoWiFi): emulate a
USB HID keyboard on ESP32-S3 and expose a HA-discoverable button that fires
`tud_remote_wakeup()` to wake the host PC from sleep.

Phase 1 status: scaffolding only. The C++ side currently compiles to a no-op.
See `~/.claude/plans/virtual-chasing-pony.md` for the implementation plan.
"""
from esphome import codegen as cg
from esphome import config_validation as cv
from esphome.const import CONF_ID
from esphome.components.esp32 import KEY_ESP32, only_on_variant
from esphome.components.esp32.const import VARIANT_ESP32S3
from esphome.core import CORE

CODEOWNERS = ["@jbjardine"]
DEPENDENCIES = ["esp32"]
MULTI_CONF = False

usb_hid_wakeup_ns = cg.esphome_ns.namespace("usb_hid_wakeup")
UsbHidWakeupComponent = usb_hid_wakeup_ns.class_("UsbHidWakeupComponent", cg.Component)


def _validate_framework(config):
    """USB HID device mode needs TinyUSB which only ships on the esp-idf framework."""
    if CORE.using_arduino:
        raise cv.Invalid(
            "usb_hid_wakeup requires the esp-idf framework. "
            "Set `esp32: framework: type: esp-idf` in your YAML."
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(UsbHidWakeupComponent),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    only_on_variant(supported=[VARIANT_ESP32S3]),
    _validate_framework,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
