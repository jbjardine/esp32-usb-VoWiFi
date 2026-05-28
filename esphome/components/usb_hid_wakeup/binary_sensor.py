"""Binary sensor platform for usb_hid_wakeup.

Exposes the USB device mount state (`tud_mounted()`) as a HA binary_sensor.
Currently only `type: mounted` is accepted — future types might include
`suspended` (USB bus suspended by host) once we wire suspend callbacks.
"""
from esphome import codegen as cg
from esphome import config_validation as cv
from esphome.components import binary_sensor

from . import UsbHidWakeupComponent, usb_hid_wakeup_ns

DEPENDENCIES = ["usb_hid_wakeup"]

CONF_USB_HID_WAKEUP_ID = "usb_hid_wakeup_id"
CONF_TYPE = "type"

TYPE_MOUNTED = "mounted"
SUPPORTED_TYPES = [TYPE_MOUNTED]

UsbHidWakeupMountedSensor = usb_hid_wakeup_ns.class_(
    "UsbHidWakeupMountedSensor", binary_sensor.BinarySensor
)

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema(UsbHidWakeupMountedSensor).extend(
    {
        cv.GenerateID(CONF_USB_HID_WAKEUP_ID): cv.use_id(UsbHidWakeupComponent),
        cv.Required(CONF_TYPE): cv.one_of(*SUPPORTED_TYPES, lower=True),
    }
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    parent = await cg.get_variable(config[CONF_USB_HID_WAKEUP_ID])
    cg.add(var.set_parent(parent))
    if config[CONF_TYPE] == TYPE_MOUNTED:
        cg.add(parent.register_mounted_sensor(var))
