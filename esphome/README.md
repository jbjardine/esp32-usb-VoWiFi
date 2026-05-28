# ESPHome `usb_hid_wakeup` component (WIP)

A would-be ESPHome external component that emulates a USB HID keyboard on
ESP32-S3 and exposes a HA-native button that triggers `tud_remote_wakeup()`
to wake the host PC from sleep.

This branch is a placeholder/scope for a 2–3 day focus project. Nothing
ESPHome-related is functional yet — the production firmware lives on
`master` as native ESP-IDF C code.

## Why this exists

The native firmware on `master` works (HID keyboard + Web UI + MQTT/HA
Discovery + OTA). It serves its purpose. But:

- Users currently configure MQTT manually (broker URL, user/password, HA
  Discovery prefix) instead of getting plug-and-play HA integration.
- The Web UI / OTA / captive portal / NVS config / mDNS are all features
  that ESPHome already provides for free with its built-in stack.
- There is no maintained ESPHome external component for USB HID keyboard
  + `tud_remote_wakeup()` on S3 → contributing one would fill a real gap.

If this works, the target user experience is **13 lines of YAML**:

```yaml
esphome:
  name: esp-wakeup

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf       # required for TinyUSB

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

external_components:
  - source: github://jbjardine/esp32-usb-VoWiFi
    path: esphome/components

usb_hid_wakeup:
  id: kbd

button:
  - platform: usb_hid_wakeup
    name: "Wakeup PC"
    usb_hid_wakeup_id: kbd
```

After flashing once, the device appears in HA via the ESPHome integration
(no MQTT broker required). One click on `button.wakeup_pc` → PC wakes.

## What needs to be built

| Piece | Where | Est. effort |
|---|---|---|
| Python config schema | `components/usb_hid_wakeup/__init__.py` | 2-3h |
| C++ Component (lifecycle + TinyUSB install) | `components/usb_hid_wakeup/usb_hid_wakeup.{h,cpp}` | 1 day |
| C++ Button platform (press → `tud_remote_wakeup`) | `components/usb_hid_wakeup/button/` | 4h |
| HID descriptor matching what `master`'s `usb.c` already produces (VID `0x303a` / PID `0x4004`) | inside C++ component | 2h |
| Resolve cohabitation with ESPHome `logger:` (which wants USB-Serial-JTAG by default on S3) | C++ component init | 4h (tricky) |
| Working example YAML compiling end-to-end on real S3 | `example/wakeup-keypress.yaml` | 2h |
| README upstream PR to esphome-components index | here + esphome/esphome PR | 2h |
| **Total** | | **~2-3 days focus** |

## References to start from

- Native code that already does what we need to port:
  - `main/usb.c` — TinyUSB init, HID descriptor, remote wakeup
  - `main/main.h` — Kconfig-driven VID/PID/strings
- ESPHome component skeleton & conventions:
  - <https://esphome.io/components/external_components.html>
  - <https://developers.esphome.io/architecture/components/> (lifecycle)
  - <https://github.com/esphome/esphome/tree/dev/esphome/components/button> (button base class to copy from)
- Similar (but not identical) projects:
  - <https://github.com/esphome/esphome/tree/dev/esphome/components/usb_uart> — USB host serial, helpful for understanding how ESPHome integrates TinyUSB
  - <https://github.com/esphome/esphome/issues?q=remote+wakeup> — open issues on HID + wakeup, none merged
- TinyUSB 2.x docs (we hit this on master already, gotchas captured in
  `~/.claude/projects/-Users-jbjardine-Documents-GitHub-esp32-usb-VoWiFi/memory/esp_idf_v6_migration_gotchas.md`)

## Non-goals on this branch

- **Do not** port the Web UI / captive portal / NVS config to ESPHome —
  ESPHome already covers all of that natively. We're only contributing
  the USB HID wakeup piece.
- **Do not** maintain the MQTT discovery code path here — ESPHome's
  native API replaces it.
- **Do not** rip out the ESP-IDF code on `master`. That stays as the
  reference implementation and as a fallback for users who can't or
  don't want to use ESPHome.

## Open questions to resolve before starting

1. Does ESPHome's `logger:` work on S3 when TinyUSB has claimed the USB
   OTG port? Likely yes if logger uses UART0 GPIO 43/44, but needs to be
   verified — the user's DevKit has a CH343 external UART that solves
   this, but not all S3 boards do.
2. Is there an existing community PR or fork to base the work on? Check
   <https://github.com/esphome/esphome/pulls?q=is%3Apr+hid+wakeup> first.
3. HID Remote Wakeup is a USB descriptor flag (`bmAttributes` bit 5).
   ESPHome's TinyUSB integration may or may not surface it — to check.
