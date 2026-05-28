"""Button platform for usb_hid_wakeup.

Press action triggers `tud_remote_wakeup()` on the parent UsbHidWakeupComponent.
"""
from esphome import codegen as cg
from esphome import config_validation as cv
from esphome.components import button
from esphome.const import CONF_ID

from . import UsbHidWakeupComponent, usb_hid_wakeup_ns

DEPENDENCIES = ["usb_hid_wakeup"]

CONF_USB_HID_WAKEUP_ID = "usb_hid_wakeup_id"

UsbHidWakeupButton = usb_hid_wakeup_ns.class_("UsbHidWakeupButton", button.Button)

CONFIG_SCHEMA = button.button_schema(UsbHidWakeupButton).extend(
    {
        cv.GenerateID(CONF_USB_HID_WAKEUP_ID): cv.use_id(UsbHidWakeupComponent),
    }
)


async def to_code(config):
    var = await button.new_button(config)
    parent = await cg.get_variable(config[CONF_USB_HID_WAKEUP_ID])
    cg.add(var.set_parent(parent))
