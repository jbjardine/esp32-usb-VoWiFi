# ESP Wakeup Keypress

A firmware for **ESP32-S3** that acts as a USB HID keyboard and remotely wakes
up the computer it is plugged into when triggered over the network — via HTTP,
the Web UI, or Home Assistant (MQTT).

The story: my PC won't power on for Wake-on-LAN magic packets (Linux/UEFI
combo I never managed to fix). ESP Wakeup Keypress sidesteps that by emulating
a USB keyboard and sending the USB *Remote Wakeup* signal: every BIOS that
honors "Wake from USB" will react.

## What it does

- Plug the ESP32-S3 into a USB port of the target PC. Most BIOSes keep USB
  powered even when the PC is asleep / off-with-5V-standby, so the ESP stays
  alive.
- The ESP connects to your home WiFi and exposes:
  - A **Web UI** (`http://<hostname>.local/` or `http://<ip>/`) with auth, a
    "Wakeup PC" button, configuration forms, and OTA updates.
  - A **plain HTTP endpoint** (`/wakeup?pass=…`) for scripts / HA REST shell
    commands / curl-from-anywhere.
  - An optional **MQTT integration** that publishes
    [HA Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery)
    configs, so a device with a Wakeup button + telemetry sensors appears in
    Home Assistant automatically.

## Hardware

- **ESP32-S3** (USB OTG natif, required — chips without USB OTG like ESP32-C3
  / C6 / H2 cannot emulate HID natively).
- Any S3 dev board works. The bundled `Kconfig` defaults to GPIO 48 for the
  on-board WS2812 LED (used by the DevKitC). DevKitC-1 has it on GPIO 38;
  change `CONFIG_ESP_WAKEUP_KEYPRESS_LED_STRIP_GPIO_NUM` in `sdkconfig` once
  before the first flash if needed.
- 8 MB flash recommended (the partition layout uses 2×1920 KB OTA slots).

## First-time build

The project keeps ESP-IDF as a git submodule for reproducibility.

```bash
git clone https://github.com/jbjardine/esp32-usb-VoWiFi.git
cd esp32-usb-VoWiFi
./00-init.sh                  # fetches submodules + installs ESP-IDF v6.0.1 toolchain
./00-set-target.sh esp32s3    # one-time target selection
./01-build.sh                 # CMake + ninja build
```

Then flash once via USB:

```bash
echo /dev/cu.usbmodem101 > defport-flash      # adapt to your serial port
./02-flash.sh
./03-monitor.sh                                # optional, serial logs
```

> macOS users: if SSL fails during the toolchain download, `pip install --user
> certifi` and re-run `./00-init.sh` with
> `export SSL_CERT_FILE=$(python3 -c "import certifi; print(certifi.where())")`.

## Provisioning workflow (after first flash)

The ESP boots and goes through this state machine:

```
boot → read NVS → has wifi creds?
        ├─ yes → try STA (3 attempts, 15s each)
        │         ├─ success → STA mode, admin UI on http://<hostname>.local/
        │         └─ all failed → fall back to SoftAP
        └─ no  → SoftAP directly
```

**SoftAP mode**: the device exposes an open WiFi `ESP-Wakeup-XXXX` (XXXX =
last 2 MAC bytes). Connect a phone to it — the OS detects a captive portal and
opens `http://192.168.4.1/setup`. Enter your home WiFi SSID + password, hit
save, the ESP reboots into STA mode.

**STA mode**: the device joins your home WiFi and serves the admin Web UI.
Username `admin`, default password `wakeup`. **Change the password from
the UI immediately.** mDNS resolves the configured hostname (default
`esp-wakeup-keypress`) as `<hostname>.local`.

If the WiFi network later disappears (router reboot, password change,
travel), the ESP retries 3× then falls back to SoftAP so you can reconfigure
without re-flashing.

## Triggering a wakeup

Three ways:

1. **Web UI**: open `/`, login, click "Wakeup PC".
2. **HTTP API for scripts / HA REST**:
   ```
   curl http://esp-wakeup-keypress.local/wakeup?pass=<wakeup-password>
   ```
3. **MQTT (if enabled)**: publish `PRESS` to the
   `esp-wakeup-XXYYZZ/cmd/wakeup` topic, or click the auto-discovered
   `button.wakeup_pc` entity in Home Assistant.

Two distinct passwords:
- The **wakeup password** is used by the legacy URL query method (above).
- The **admin password** protects the Web UI + REST `/api/*` endpoints via
  HTTP Basic Auth.

Both default to `wakeup` on first boot and are stored as SHA-256 hashes in
NVS — change them from the admin UI.

## Home Assistant integration

1. In the admin UI, fill the MQTT section (broker URL, user/password) and
   check "Activer MQTT". Save and reboot.
2. The ESP connects to your broker, publishes its discovery configs, and HA
   creates a device with the following entities:

   | Entity | Type | What |
   |---|---|---|
   | `button.wakeup_pc` | Button | Click to fire a USB remote wakeup |
   | `binary_sensor.usb_connected` | Connectivity | USB link to PC up/down |
   | `sensor.rssi` | Signal strength | WiFi RSSI, refreshed every 30s |
   | `sensor.uptime` | Duration | Seconds since boot |
   | `sensor.ip` | Diagnostic | Current LAN IP |
   | `sensor.wakeup_count` | Counter | Total wakeups fired since boot |

3. Availability is tracked via MQTT LWT — when the ESP loses power / crashes,
   HA marks the device unavailable within ~30s.

## Over-the-air updates

Once the ESP is in STA mode and you have admin access, you no longer need to
physically reflash:

1. Rebuild locally: `./01-build.sh`
2. In the admin UI, scroll to the **OTA** section, pick
   `build/esp-wakeup-keypress.bin`, hit "Flasher le firmware".
3. The browser uploads the binary; the ESP writes it to the inactive OTA slot,
   flips the boot target, and reboots.
4. After 30 s of healthy uptime on the new image, the bootloader marks it
   valid. If it crashes before 30 s (broken build, missing peripheral, etc.),
   the rollback partition automatically takes back over on the next boot.

## Troubleshooting wake from sleep

If your computer **enumerates** the keyboard but won't wake from sleep, the
kernel may have wakeup disabled for the device. On Linux create
`/lib/systemd/system-sleep/00-esp-wakeup-enable.sh`:

```bash
#!/bin/bash
# Re-enable wakeup on the ESP USB device after each suspend/resume cycle.
if [ "$1" = post ] || [ "$1" = pre ]; then
    KB="$(lsusb -tvv | grep -A 1 303a:4004 | awk 'NR==2 {print $1}')"
    echo enabled > "${KB}/power/wakeup"
fi
```

`303a:4004` is the default Espressif USB VID:PID; adjust if you changed it in
`usb.c`. Make the script executable (`chmod +x`).

## Architecture

```
                          ┌────────────────────────────────────────────┐
                          │              ESP32-S3                       │
                          │  ┌──────────────────────────────────────┐  │
   Phone/Laptop ─HTTP───▶ │  │ httpd (esp_http_server)              │  │
                          │  │  ├ /          page wakeup/setup      │  │
                          │  │  ├ /admin/*   WiFi/pwd/MQTT/OTA      │  │
                          │  │  ├ /api/*     status / wakeup JSON   │  │
                          │  │  └ /wakeup    legacy GET ?pass=...   │  │
                          │  └──────────────────────────────────────┘  │
   HA (Mosquitto) ◀MQTT─▶ │  ┌──────────────────────────────────────┐  │
                          │  │ mqtt.c : LWT + HA Discovery + telem  │  │
                          │  └──────────────────────────────────────┘  │
                          │  ┌──────────────────────────────────────┐  │
                          │  │ wifi.c : STA → fallback SoftAP       │  │
                          │  │ captive_portal.c : DNS hijack /53    │  │
                          │  └──────────────────────────────────────┘  │
                          │  ┌──────────────────────────────────────┐  │
   PC éteint ◀═USB═════ ▶ │  │ usb.c : TinyUSB HID + remote wakeup  │  │
                          │  └──────────────────────────────────────┘  │
                          │  ┌──────────────────────────────────────┐  │
                          │  │ nvs_config.c : creds, hashes, MQTT,  │  │
                          │  │                hostname              │  │
                          │  └──────────────────────────────────────┘  │
                          └────────────────────────────────────────────┘
```

Source files in `main/` :
- `main.c` — init order
- `usb.c` — TinyUSB HID keyboard with remote wakeup
- `wifi.c` — STA + fallback SoftAP
- `captive_portal.c` — DNS hijack for captive portal detection
- `httpd.c` — all HTTP routes
- `auth.c` — Basic Auth verification (SHA-256 + constant-time compare)
- `mqtt.c` — MQTT client + HA Discovery
- `ota.c` — OTA update streaming + rollback validation
- `nvs_config.c` — persistent configuration (WiFi creds, password hashes,
  MQTT, hostname)
- `gpio.c` / `led.c` / `led_strip_encoder.c` — button + RGB LED feedback
- `web/*.html` — embedded admin and setup UIs (compiled into the firmware)

## Stack

- ESP-IDF **v6.0.1** (LTS)
- TinyUSB via `espressif/esp_tinyusb` 2.2.0
- MQTT via `espressif/mqtt` (managed component)
- mbedtls 4.x + PSA Crypto API for SHA-256 and constant-time compares

## License

MIT, see `LICENSE`.
