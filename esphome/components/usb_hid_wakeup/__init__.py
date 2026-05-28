"""USB HID Wakeup external component for ESPHome.

Emulates a USB HID keyboard on ESP32-S3 (esp-idf + esp_tinyusb) and exposes
Home-Assistant-native entities:
  - button (type: wakeup)         -> tud_remote_wakeup()
  - button (type: force_shutdown) -> Win+R "shutdown /s /f /t 0" + ACPI power down
  - binary_sensor (type: mounted / suspended)

Descriptors ported from the native firmware on `master`
(https://github.com/jbjardine/esp32-usb-VoWiFi). See README.md.
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

# esp_tinyusb 2.2.0 matches what the native firmware on `master` validated.
ESP_TINYUSB_REF = "~2.2.0"

CONF_VID = "vid"
CONF_PID = "pid"
CONF_MANUFACTURER = "manufacturer"
CONF_PRODUCT = "product"
CONF_SERIAL = "serial"
CONF_KEYBOARD_LAYOUT = "keyboard_layout"

usb_hid_wakeup_ns = cg.esphome_ns.namespace("usb_hid_wakeup")
UsbHidWakeupComponent = usb_hid_wakeup_ns.class_("UsbHidWakeupComponent", cg.Component)

KeyboardLayout = usb_hid_wakeup_ns.enum("KeyboardLayout")
KEYBOARD_LAYOUTS = {
    "azerty": KeyboardLayout.LAYOUT_AZERTY,
    "us": KeyboardLayout.LAYOUT_US,
}


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
            cv.Optional(CONF_VID, default=0x303A): cv.hex_uint16_t,
            cv.Optional(CONF_PID, default=0x4004): cv.hex_uint16_t,
            cv.Optional(CONF_MANUFACTURER, default="ESP"): cv.All(
                cv.string, cv.Length(max=63)
            ),
            cv.Optional(CONF_PRODUCT, default="Wakeup Keyboard Device"): cv.All(
                cv.string, cv.Length(max=63)
            ),
            cv.Optional(CONF_SERIAL, default="123456"): cv.All(
                cv.string, cv.Length(max=63)
            ),
            cv.Optional(CONF_KEYBOARD_LAYOUT, default="azerty"): cv.enum(
                KEYBOARD_LAYOUTS, lower=True
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    only_on_variant(supported=[VARIANT_ESP32S3]),
    _validate_framework,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_usb_vid(config[CONF_VID]))
    cg.add(var.set_usb_pid(config[CONF_PID]))
    cg.add(var.set_manufacturer(config[CONF_MANUFACTURER]))
    cg.add(var.set_product(config[CONF_PRODUCT]))
    cg.add(var.set_serial(config[CONF_SERIAL]))
    cg.add(var.set_keyboard_layout(config[CONF_KEYBOARD_LAYOUT]))

    # No ESPHome `tinyusb` component exists, so we own all tud_* callbacks with
    # no link conflict. Pull TinyUSB in as a managed IDF component.
    esp32.add_idf_component(name="espressif/esp_tinyusb", ref=ESP_TINYUSB_REF)

    esp32.add_idf_sdkconfig_option("CONFIG_TINYUSB_HID_COUNT", 1)
    esp32.add_idf_sdkconfig_option("CONFIG_TINYUSB_CDC_COUNT", 0)
