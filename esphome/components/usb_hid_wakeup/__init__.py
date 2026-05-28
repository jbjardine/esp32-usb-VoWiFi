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
from esphome.components import esp32
from esphome.components.esp32 import only_on_variant
from esphome.components.esp32.const import VARIANT_ESP32S3
from esphome.core import CORE

CODEOWNERS = ["@jbjardine"]
DEPENDENCIES = ["esp32"]
MULTI_CONF = False

# esp_tinyusb 2.2.0 matches what the native firmware on `master` validated:
# nested tinyusb_config_t.descriptor layout + TINYUSB_TASK_DEFAULT() macro.
ESP_TINYUSB_REF = "~2.2.0"

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

    # Pull TinyUSB in as a managed IDF component (no ESPHome tinyusb component
    # exists, so we own all the tud_* descriptor/HID callbacks with no conflict).
    esp32.add_idf_component(name="espressif/esp_tinyusb", ref=ESP_TINYUSB_REF)

    # CFG_TUD_HID is driven by this Kconfig knob; without it tud_hid_* and the
    # HID interface buffers are not compiled in.
    esp32.add_idf_sdkconfig_option("CONFIG_TINYUSB_HID_COUNT", 1)
    # We supply our own descriptors via tinyusb_config_t, but TinyUSB still
    # compiles its default string table from these — keep them sane.
    esp32.add_idf_sdkconfig_option("CONFIG_TINYUSB_CDC_COUNT", 0)
