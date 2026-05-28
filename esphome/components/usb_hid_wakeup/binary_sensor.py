"""Binary sensor platform for usb_hid_wakeup.

Types:
  - awake:     mounted && !suspended -> PC powered AND not asleep == usable now.
               The clean "is my PC on" reading (cannot tell sleep from full off).
               Pair with `device_class: running` for "En marche"/"Arrêté" labels.
  - mounted:   tud_mounted()    -> USB enumerated to a host (connectivity)
  - suspended: tud_suspended()  -> USB bus suspended == host asleep/off (passive)
"""
from esphome import codegen as cg
from esphome import config_validation as cv
from esphome.components import binary_sensor

from . import UsbHidWakeupComponent, usb_hid_wakeup_ns

DEPENDENCIES = ["usb_hid_wakeup"]

CONF_USB_HID_WAKEUP_ID = "usb_hid_wakeup_id"
CONF_TYPE = "type"

TYPE_AWAKE = "awake"
TYPE_MOUNTED = "mounted"
TYPE_SUSPENDED = "suspended"

UsbHidWakeupAwakeSensor = usb_hid_wakeup_ns.class_(
    "UsbHidWakeupAwakeSensor", binary_sensor.BinarySensor
)
UsbHidWakeupMountedSensor = usb_hid_wakeup_ns.class_(
    "UsbHidWakeupMountedSensor", binary_sensor.BinarySensor
)
UsbHidWakeupSuspendedSensor = usb_hid_wakeup_ns.class_(
    "UsbHidWakeupSuspendedSensor", binary_sensor.BinarySensor
)

_PARENT = {cv.GenerateID(CONF_USB_HID_WAKEUP_ID): cv.use_id(UsbHidWakeupComponent)}

CONFIG_SCHEMA = cv.typed_schema(
    {
        TYPE_AWAKE: binary_sensor.binary_sensor_schema(
            UsbHidWakeupAwakeSensor
        ).extend(_PARENT),
        TYPE_MOUNTED: binary_sensor.binary_sensor_schema(
            UsbHidWakeupMountedSensor
        ).extend(_PARENT),
        TYPE_SUSPENDED: binary_sensor.binary_sensor_schema(
            UsbHidWakeupSuspendedSensor
        ).extend(_PARENT),
    },
    lower=True,
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    parent = await cg.get_variable(config[CONF_USB_HID_WAKEUP_ID])
    cg.add(var.set_parent(parent))
    if config[CONF_TYPE] == TYPE_AWAKE:
        cg.add(parent.register_awake_sensor(var))
    elif config[CONF_TYPE] == TYPE_MOUNTED:
        cg.add(parent.register_mounted_sensor(var))
    else:
        cg.add(parent.register_suspended_sensor(var))
