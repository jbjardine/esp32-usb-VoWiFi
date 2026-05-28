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

## Force-shutdown a Windows PC (v0.2)

`button:` with `type: force_shutdown` forces the **Windows** host to power off,
even with apps/sessions open. There is no SysRq-style un-blockable kernel
sequence on Windows, so this combines two legs in one press:

1. **Unlocked session (true force)**: `Win+R` → types `shutdown /s /f /t 0` →
   `Enter`. The `/f` flag force-closes apps without "save your work" prompts.
2. **Locked session (graceful fallback)**: an ACPI **System Power Down** HID
   usage (works at the lock screen, but apps can delay/block it).

### Keyboard layout matters

HID keycodes are positional. To type the command correctly on a **French
AZERTY** Windows host you MUST set `keyboard_layout: azerty` (default). `us` is
also supported. Other layouts: type the command via the `type_test` button into
Notepad and check — if wrong, open an issue.

### Validate safely before trusting it

Add a `type: type_test` button. Open **Notepad** on the host, focus it, press
the button: it types `shutdown /s /f /t 0` with **no Win+R, no Enter, no power
down**. Confirm the text is exactly right (watch `w`, `/`, `0`) before ever
pressing `force_shutdown`.

### Windows prerequisites

- For the **locked** leg: Control Panel → Power Options → "Choose what the power
  button does" → set to **Shut down** (otherwise System Power Down sleeps).
- Recommended: enable a **confirmation dialog** on the HA button card
  (`tap_action`) to avoid an accidental shutdown click.
- The signed-in user needs shutdown privilege (default on desktop installs).

> Linux/macOS are not targeted by `force_shutdown` (the typed command and the
> ACPI semantics are Windows-specific). The `wakeup` button is OS-agnostic.

## Configurable USB identity (v0.2)

`vid`, `pid`, `manufacturer`, `product`, `serial` are optional on the
`usb_hid_wakeup:` block (defaults match the native firmware: Espressif VID
`0x303A`, PID `0x4004`). Useful to disambiguate multiple devices or to satisfy a
BIOS that filters wake-capable devices by VID:PID.

## Power-state reporting (v0.2)

The recommended single entity is the **`text_sensor`** status — one diagnostic
entity with three human-readable states:

```yaml
text_sensor:
  - platform: usb_hid_wakeup
    name: "PC status"
    usb_hid_wakeup_id: kbd
    entity_category: diagnostic
```

| Value | Meaning |
|---|---|
| `Awake` | host powered and not asleep — usable now |
| `Asleep or off` | USB bus suspended |
| `Disconnected` | no USB host (unplugged / no standby power) |

⚠️ USB alone **cannot distinguish sleep (S3) from full shutdown (S5)** — both
stop bus activity, hence the combined `Asleep or off`. Telling them apart would
require an agent on the PC, not this device.

If you prefer separate on/off entities, three `binary_sensor` types are also
available (all passive):

| `type` | True when | Suggested `device_class` | French label |
|---|---|---|---|
| `awake` | `mounted && !suspended` | `running` | En marche / Arrêté |
| `mounted` | USB enumerated to a host | `connectivity` | Connecté / Déconnecté |
| `suspended` | USB bus suspended | — | Activé / Désactivé |

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

- [x] Hardware E2E validation on a real S3 (enumerate + wake a sleeping PC)
- [x] Expose VID/PID/manufacturer/product/serial as YAML options (v0.2)
- [x] `type: suspended` binary sensor (v0.2)
- [x] Windows force-shutdown via `type: force_shutdown` (v0.2)
- [ ] Hardware E2E validation of force_shutdown + AZERTY mapping
- [ ] Additional keyboard layouts beyond AZERTY/US
- [ ] Upstream PR to esphome/esphome once stable
