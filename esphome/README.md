# ESPHome `usb_hid_wakeup` component

An ESPHome external component that emulates a USB HID keyboard on ESP32-S3 and
exposes a Home-Assistant-native button that fires `tud_remote_wakeup()` to wake
the host PC from sleep — plus a binary sensor reflecting USB connection state.

**Status: MVP implemented + compile-validated** (ESPHome 2025.12.3, esp-idf
framework, `espressif/esp_tinyusb` 2.2.0). Descriptors ported byte-for-byte
from the native firmware on `master` (`main/usb.c`).

## Why this exists

The native firmware on `master` works (HID keyboard + Web UI + MQTT/HA
Discovery + OTA). But the MQTT path requires manually configuring a broker,
users and ACLs. ESPHome gives plug-and-play HA integration via its native API
(no broker), and reuses ESPHome's WiFi / OTA / captive-portal / web stack for
free. No maintained ESPHome component existed for USB HID keyboard +
`tud_remote_wakeup()` on S3 — this fills that gap.

The native ESP-IDF firmware on `master` remains the reference implementation
and the fallback for anyone who doesn't want ESPHome.

## Usage

`example/wakeup-keypress.yaml` is the full reference. Minimal config:

```yaml
esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf          # REQUIRED — TinyUSB is not on Arduino-S3

logger:
  hardware_uart: USB_SERIAL_JTAG   # safe: separate from USB OTG used by HID

external_components:
  - source: github://jbjardine/esp32-usb-VoWiFi
    components: [usb_hid_wakeup]

usb_hid_wakeup:
  id: kbd

button:
  - platform: usb_hid_wakeup
    name: "Wakeup PC"
    usb_hid_wakeup_id: kbd

binary_sensor:
  - platform: usb_hid_wakeup
    name: "USB host connected"
    type: mounted
    usb_hid_wakeup_id: kbd
    device_class: connectivity
```

Flash once, then the device shows up in HA via the ESPHome integration. One
click on `button.wakeup_pc` → the PC wakes (if it was asleep with USB wake
enabled in BIOS).

## Configuration reference

### `usb_hid_wakeup:` (component, one per device)
| Option | Type | Default | Notes |
|---|---|---|---|
| `id` | ID | generated | Reference it from the platforms below. |

No further options at MVP: VID/PID/strings are hardcoded to match the native
firmware (Espressif VID, product `"Wakeup Keyboard Device"`). Exposing them as
YAML is a planned follow-up.

### `button:` platform
| Option | Type | Default | Notes |
|---|---|---|---|
| `usb_hid_wakeup_id` | ID | — | The `usb_hid_wakeup` component to drive. |
| (all standard `button` options) | | | `name`, `icon`, `entity_category`, … |

`press` → `tud_remote_wakeup()`. No-op (with an info log) if the host is awake
or not mounted.

### `binary_sensor:` platform
| Option | Type | Default | Notes |
|---|---|---|---|
| `usb_hid_wakeup_id` | ID | — | The `usb_hid_wakeup` component to observe. |
| `type` | enum | — | Only `mounted` for now (`tud_mounted()` state). |
| (all standard `binary_sensor` options) | | | `device_class: connectivity` recommended. |

## How it differs from `master/main/usb.c`

| Native firmware | This component |
|---|---|
| FreeRTOS event group + dedicated `usb_task` | esp_tinyusb's own task pumps `tud_task()`; we only poll mount state in `loop()` |
| `usb_request_keypress_send()` sets an event bit | `UsbHidWakeupButton::press_action()` calls `request_wakeup()` directly |
| LED + MQTT side effects on wakeup | none — ESPHome handles entity state/telemetry natively |
| Kconfig-driven VID/PID/strings | hardcoded (matches the same defaults) |

Descriptors, the string table and the three `tud_hid_*` callbacks are
identical to the native firmware (`main/usb.c:16-64`), including the
`TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP` bit that authorizes remote wakeup.

## Hardware requirement

**ESP32-S3 only.** USB OTG native peripheral is required to act as a USB
device. ESP32-C3/C6/H2 have only a fixed USB-Serial-JTAG and cannot enumerate
as an arbitrary HID device. The Python schema rejects non-S3 variants and the
Arduino framework at config time.

## Troubleshooting

- **Button does nothing / PC doesn't wake**: the PC must have "Wake from USB"
  / "USB Power S5" enabled in BIOS, and the OS must allow wakeup on the device.
  On Linux, `echo enabled > /sys/.../power/wakeup` for the device (see the
  `master` README's systemd snippet). Check `esphome logs` — "remote wakeup not
  sent" means the host wasn't suspended (so nothing to wake).
- **No serial logs after boot**: HID claims the USB OTG port. Use
  `logger: hardware_uart: USB_SERIAL_JTAG` (a separate peripheral on S3) or an
  external UART. Do NOT use `hardware_uart: USB_CDC` — it conflicts with HID.
- **Reflash needs BOOT+RESET**: with HID active, the USB-Serial download path
  may be unavailable; hold BOOT, tap RESET, release BOOT to enter the ROM
  bootloader before `esphome run`.

## Roadmap

- [ ] Hardware E2E validation on a real S3 (enumerate + wake a sleeping PC)
- [ ] Expose VID/PID/manufacturer/product/serial as YAML options
- [ ] `type: suspended` binary sensor (wire `tud_suspend_cb`/`tud_resume_cb`)
- [ ] Optional keystroke action (send actual key, not just remote wakeup)
- [ ] Upstream PR to esphome/esphome once stable
