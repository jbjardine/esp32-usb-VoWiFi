"""Binary sensor platform for usb_hid_wakeup.

Two types:
  - mounted:   tud_mounted()   -> USB enumerated to a host (connectivity)
  - suspended: tud_suspended()  -> USB bus suspended == host asleep (passive)
"""
from esphome import codegen as cg
from esphome import config_validation as cv
from esphome.components import binary_sensor

from . import UsbHidWakeupComponent, usb_hid_wakeup_ns

DEPENDENCIES = ["usb_hid_wakeup"]

CONF_USB_HID_WAKEUP_ID = "usb_hid_wakeup_id"
CONF_TYPE = "type"

TYPE_MOUNTED = "mounted"
TYPE_SUSPENDED = "suspended"
SUPPORTED_TYPES = [TYPE_MOUNTED, TYPE_SUSPENDED]

UsbHidWakeupMountedSensor = usb_hid_wakeup_ns.class_(
    "UsbHidWakeupMountedSensor", binary_sensor.BinarySensor
)
UsbHidWakeupSuspendedSensor = usb_hid_wakeup_ns.class_(
    "UsbHidWakeupSuspendedSensor", binary_sensor.BinarySensor
)

CONFIG_SCHEMA = cv.typed_schema(
    {
        TYPE_MOUNTED: binary_sensor.binary_sensor_schema(
            UsbHidWakeupMountedSensor
        ).extend(
            {cv.GenerateID(CONF_USB_HID_WAKEUP_ID): cv.use_id(UsbHidWakeupComponent)}
        ),
        TYPE_SUSPENDED: binary_sensor.binary_sensor_schema(
            UsbHidWakeupSuspendedSensor
        ).extend(
            {cv.GenerateID(CONF_USB_HID_WAKEUP_ID): cv.use_id(UsbHidWakeupComponent)}
        ),
    },
    lower=True,
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    parent = await cg.get_variable(config[CONF_USB_HID_WAKEUP_ID])
    cg.add(var.set_parent(parent))
    if config[CONF_TYPE] == TYPE_MOUNTED:
        cg.add(parent.register_mounted_sensor(var))
    else:
        cg.add(parent.register_suspended_sensor(var))
