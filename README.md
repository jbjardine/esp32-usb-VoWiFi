# ESP Wakeup Keypress

Turn an **ESP32-S3** into a USB device plugged into a PC that can **wake it
from sleep** (and, on Windows, **force-shut-it-down**) remotely — emulating a
USB HID keyboard and sending the USB *Remote Wakeup* signal. Useful when
Wake-on-LAN is unreliable on the target machine.

> The repo is named `esp32-usb-VoWiFi` for historical reasons — it has nothing
> to do with Voice-over-WiFi. It's a USB HID wakeup keyboard.

## Two ways to use it

### 1. ESPHome component — recommended

A self-contained ESPHome external component in **[`esphome/`](esphome/)**.
Plug-and-play Home Assistant integration over the native ESPHome API (no MQTT
broker), and you reuse ESPHome's WiFi / OTA / captive-portal stack for free.

```yaml
external_components:
  - source: github://jbjardine/esp32-usb-VoWiFi
    components: [usb_hid_wakeup]

usb_hid_wakeup:
  id: kbd

button:
  - platform: usb_hid_wakeup
    type: wakeup
    name: "Wakeup PC"
    usb_hid_wakeup_id: kbd
```

Exposes: `wakeup` / `force_shutdown` / `acpi_shutdown` / `auto_shutdown` /
`type_test` buttons, a `PC status` diagnostic text sensor, and
`awake`/`mounted`/`suspended` binary sensors. Configurable USB identity and
keyboard layout (AZERTY/US). Full docs: **[`esphome/README.md`](esphome/README.md)**.

### 2. Native ESP-IDF firmware — advanced

A full ESP-IDF v6 firmware in **[`native-firmware/`](native-firmware/)** with a
self-hosted Web UI (WiFi provisioning + admin), MQTT + Home Assistant
Discovery, OTA web updates, captive-portal fallback and mDNS. Heavier, but
standalone (no Home Assistant required). Docs:
**[`native-firmware/README.md`](native-firmware/README.md)**.

## Which one?

| You want… | Use |
|---|---|
| Plug-and-play HA integration, minimal config | **ESPHome** (`esphome/`) |
| Standalone device with its own Web UI / MQTT / OTA, no HA needed | **Native** (`native-firmware/`) |

Both target **ESP32-S3** (USB OTG native peripheral required — C3/C6/H2 can't
emulate an arbitrary HID device).

## License

MIT, see [`LICENSE`](LICENSE).
