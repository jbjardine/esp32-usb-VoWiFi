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

Shutting down a **Windows** host has no SysRq-style un-blockable kernel
sequence, and the two available mechanisms each only fit one lock state. So
they are exposed as **three explicit button types** — pick per situation:

| `button` `type` | What it does | Use when |
|---|---|---|
| `force_shutdown` | `Win+R` → types `shutdown /s /f /t 0` → `Enter`. `/f` force-closes apps. | session **unlocked** (true force) |
| `acpi_shutdown` | ACPI **System Power Down** HID usage only, no typing | session **locked** (graceful) |
| `auto_shutdown` | force macro then ACPI fallback, one press | covers both, but stateful |

There is also a `sleep` button type: ACPI **System Sleep** (puts the PC into
S3 sleep). It's the counterpart of `wakeup` and works on any OS that sleeps on
the System Sleep HID usage (default on Windows).

⚠️ `force_shutdown` on a **locked** screen does nothing useful — `Win+R` is
blocked and the typed characters land in the password field (one failed login
attempt). Use `acpi_shutdown` when locked. `acpi_shutdown` is **graceful**
(apps may delay it) and needs the Windows power-button action set to
**Shut down** (below).

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

| Value | `mounted` | `suspended` | Meaning |
|---|---|---|---|
| `Allumé` | yes | no | PC on, USB bus active |
| `Veille ou éteint` | yes | yes | bus suspended — **S3 sleep OR S5 off** (see below) |
| `Débranché` | no | — | de-enumerated — cable unplugged / no standby power |

⚠️ **Sleep (S3) and off (S5) are often indistinguishable to the device.** On a
board that keeps USB powered in S5 for "power-on-by-keyboard", a fully-off PC
stays enumerated with the bus suspended — exactly like sleep — so both read as
`Veille ou éteint`. Only a board that *cuts* USB power in S5 would show
`Débranché` when off. There is no reliable USB-only way to tell them apart;
distinguishing them would need an agent on the PC.

If you prefer separate on/off entities, three `binary_sensor` types are also
available (all passive):

| `type` | True when | Suggested `device_class` | French label |
|---|---|---|---|
| `awake` | `mounted && !suspended` | `running` | En marche / Arrêté |
| `mounted` | USB enumerated to a host | `connectivity` | Connecté / Déconnecté |
| `suspended` | USB bus suspended | — | Activé / Désactivé |

## Onboard RGB status LED (optional)

The S3's onboard WS2812 can show device state at a glance. This is **100% YAML**
— the component just publishes state, the LED is presentation. Declare the LED
as a `light:` and drive it from automations keyed on `PC status` + the buttons +
WiFi state. Full block in [`example/wakeup-keypress.yaml`](example/wakeup-keypress.yaml).

| State | LED |
|---|---|
| PC `Allumé` | 🟢 green (dim) |
| PC `Veille` | 🟠 amber (dim) |
| PC `Éteint` | 🔴 red (dim) |
| Wakeup pressed | ⚪ white flash → back to state color |
| Shutdown pressed | 🟣 purple flash → back to state color |
| WiFi lost / AP mode | 🔵 blinking blue (overrides — device unreachable) |

```yaml
light:
  - platform: esp32_rmt_led_strip
    id: status_led
    internal: true
    pin: GPIO48          # DevKitC-1 onboard WS2812 (some boards: GPIO38)
    num_leds: 1
    chipset: WS2812
    rgb_order: GRB
    effects:
      - pulse: { name: wifi_lost, update_interval: 600ms, min_brightness: 0%, max_brightness: 30% }

script:
  - id: led_show_state
    then:
      - if:
          condition: { not: { wifi.connected: } }
          then:
            - light.turn_on: { id: status_led, blue: 100%, red: 0%, green: 0%, effect: wifi_lost }
          else:
            - lambda: |-
                auto call = id(status_led).turn_on();
                call.set_effect("None"); call.set_brightness(0.15f);
                const std::string &s = id(pc_status).state;
                if (s == "Allumé")      call.set_rgb(0,1,0);
                else if (s == "Veille") call.set_rgb(1,0.5f,0);
                else                    call.set_rgb(1,0,0);
                call.perform();
```

Then give the `text_sensor` `id: pc_status` + `on_value: [script.execute: led_show_state]`,
add `on_connect`/`on_disconnect` on `wifi:` and `on_boot` on `esphome:` (all
`script.execute: led_show_state`), and a white/purple `light.turn_on` + `delay`
+ `script.execute: led_show_state` on each button's `on_press`.

> Don't also declare ESPHome's `status_led:` on GPIO48 — we own the LED here.
> Adjust `pin:` if your board's WS2812 is on a different GPIO (e.g. 38).

## Hardware requirement

**ESP32-S3 only.** USB OTG native peripheral is required to act as a USB
device. ESP32-C3/C6/H2 have only a fixed USB-Serial-JTAG and cannot enumerate
as an arbitrary HID device. The Python schema rejects non-S3 variants and the
Arduino framework at config time.

## Waking the PC: S3 vs S5, and the post-OTA gotcha

The `wakeup` button calls `tud_remote_wakeup()`, which emits USB *resume*
signalling. How the PC reacts depends on its sleep state — and on the host
having **armed/registered** this device for wake:

- **From sleep (S3)** — bus is suspended but the device stays enumerated. The
  OS resumes on the signal **if** it armed remote-wakeup for the device. Windows
  arms it when the device is enumerated as wake-capable and the PC then sleeps.
  Make it explicit: Device Manager → the keyboard device → Power Management →
  **"Allow this device to wake the computer"**.
- **From full off (S5)** — waking needs a BIOS feature (**"Power On By
  Keyboard/USB" / "Wake on USB S4-S5"**). Test with a real keyboard: if a
  keypress powers the PC on, the feature is enabled and the ESP can do it too
  (the same `tud_remote_wakeup()` resume signalling is detected by the chipset's
  wake logic). On such boards USB stays powered in S5 and the device stays
  enumerated+suspended, so the status shows `Veille ou éteint` (not
  `Débranché`) — sleep and off look identical. Boards that disable USB standby
  power in S5 (ErP/EuP) can't wake from USB at all and show `Débranché` when off.

### ⚠️ After every OTA flash: do one full PC power cycle

An OTA flash soft-reboots the ESP, which **re-enumerates** the USB device. But
the wake registration is snapshotted by the host at a *full* cycle, not on a
soft re-enumeration:

- **S3 / Windows**: the OS may keep the old (now-dead) device handle and not
  re-arm wake until a fresh enumeration — physically re-plug the OTG cable, or
  do one sleep/wake cycle.
- **S5 / BIOS**: the firmware's "power-on-by-keyboard" allow-list is captured at
  **boot/shutdown**, not at runtime. After an OTA flash the ESP isn't on that
  list until the next full cycle.

**Fix for both:** after flashing, do **one full PC boot → shutdown** (and/or
re-plug the OTG cable). The host re-registers the ESP keyboard, and wake from
both S3 and S5 works again. This is expected, not a bug.

## Troubleshooting

- **Button does nothing / PC doesn't wake**: see the section above — most often
  the host hasn't (re-)armed wake after an OTA flash; do one full PC power
  cycle. Also confirm BIOS "Wake/Power-On from USB" and the OS "allow this
  device to wake" setting. Check `esphome logs` — "remote wakeup not sent"
  means the host wasn't suspended/armed.
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
