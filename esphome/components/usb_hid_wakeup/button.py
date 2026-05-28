"""Button platform for usb_hid_wakeup.

Button types:
  - wakeup (default): tud_remote_wakeup() to wake a sleeping host
  - force_shutdown:   Win+R "shutdown /s /f /t 0" macro + ACPI power down (Windows)
  - type_test:        SAFE — types the shutdown command into the focused window
                      (no Win+R, no Enter, no power down) to validate keyboard_layout
"""
from esphome import codegen as cg
from esphome import config_validation as cv
from esphome.components import button

from . import UsbHidWakeupComponent, usb_hid_wakeup_ns

DEPENDENCIES = ["usb_hid_wakeup"]

CONF_USB_HID_WAKEUP_ID = "usb_hid_wakeup_id"
CONF_TYPE = "type"

ButtonAction = usb_hid_wakeup_ns.enum("ButtonAction")
TYPES = {
    "wakeup": ButtonAction.ACTION_WAKEUP,
    "force_shutdown": ButtonAction.ACTION_FORCE_SHUTDOWN,
    "type_test": ButtonAction.ACTION_TYPE_TEST,
}

UsbHidWakeupButton = usb_hid_wakeup_ns.class_("UsbHidWakeupButton", button.Button)

CONFIG_SCHEMA = button.button_schema(UsbHidWakeupButton).extend(
    {
        cv.GenerateID(CONF_USB_HID_WAKEUP_ID): cv.use_id(UsbHidWakeupComponent),
        cv.Optional(CONF_TYPE, default="wakeup"): cv.enum(TYPES, lower=True),
    }
)


async def to_code(config):
    var = await button.new_button(config)
    parent = await cg.get_variable(config[CONF_USB_HID_WAKEUP_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_action(config[CONF_TYPE]))
