# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A two-part project for an ESP32-C3-based 3D-printer-farm status light:

1. **Web flasher** (`index.html`, `src/main.js`, `src/style.css`) — a static Vite/vanilla-JS site. Uses `esptool-js` over the Web Serial API to flash a pre-built firmware binary, then opens a plain serial link to provision WiFi + server config via JSON.
2. **ESP32-C3 firmware** (`firmware/status-light/status-light.ino`, `config.h`) — an Arduino sketch that connects to WiFi and reflects a printer's state on a discrete RGB LED. The status source is **chosen at provisioning via a `mode` field**: `mqtt` (default) **subscribes** to the dashboard's embedded MQTT broker (`printfarm/printers/<printerId>/status`, pushed), or `api` **polls** `GET <serverUrl>/api/status-light/printers/<printerId>` over plain HTTP(S). Both talk to the same dashboard.

The site is deployed as a static site (Vercel config in `vercel.json`); the firmware binary it serves lives at `public/firmware/status-light.bin`.

## Commands

```bash
npm install
npm run dev       # Vite dev server on :3000 (opens browser automatically)
npm run build     # outputs to dist/
npm run preview   # serve the production build locally
```

There is no test suite or linter configured. `npm run build` is the only automated verification for the web app.

Web Serial requires a secure context (HTTPS, or `https://localhost`/browser flag for local dev) and a user gesture to pick a device — you cannot drive the flash flow from a script.

## Building the firmware binary

**`public/firmware/status-light.bin` is not committed to git** (see `.gitignore`) and does not exist on a fresh checkout. If it's missing, Vite's dev-server SPA fallback serves `index.html` for that path with a `200`, and `src/main.js`'s magic-byte check (`0xe9`) catches it and reports "not a valid ESP firmware image" rather than flashing garbage.

`arduino-cli` is not assumed to be installed; use PlatformIO instead:

A `platformio.ini` now lives in `firmware/status-light/` (with `src_dir = .`, so the single `.ino` in that folder is compiled directly — no "copy to `src/main.cpp`" step, and the sketch already declares all its function prototypes up front). Just:

```bash
cd firmware/status-light
pio run
```

- `platformio.ini` pins `board = esp32-c3-devkitm-1`, `framework = arduino`, `lib_deps = bblanchon/ArduinoJson@^7`, and **must** set `build_flags = -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1` — most ESP32-C3 "Super Mini" boards use the chip's native USB Serial/JTAG (enumerates as `303a:1001`, e.g. `/dev/ttyACM0`). Without `CDC_ON_BOOT`, the firmware's `Serial` goes to UART0 and the browser never sees any output. (The MQTT client (esp-mqtt) and public-CA bundle (`esp_crt_bundle`) are part of the ESP-IDF bundled with the Arduino-ESP32 core — no extra `lib_deps`; the LED is driven with plain `digitalWrite`, so no LED library either.)
- The post-build hook `scripts/merge_firmware.py` merges bootloader + partition table + `boot_app0` + app into a single image at offset `0x0` (a bare app `.bin` at `0x10000` alone won't boot from `0x0`) and writes it straight to `../../public/firmware/status-light.bin` — no manual copy. The `.pio/` build tree is git-ignored.

`public/firmware/manifest.json` (esp-web-tools format) declares the chip family (`ESP32-C3`) and the single part (`status-light.bin` @ offset `0`) the flasher writes.

> Building the ESP toolchain needs normal internet access to PlatformIO's package registry (`api.registry.platformio.org`) and Espressif's download servers — a restricted-egress CI/sandbox can't fetch them.

## Web flasher architecture (`src/main.js`)

Single state machine driving four UI panels/steps: **connect → flash → provision → done**. All state is module-level (`port`, `transport`, `esploader`, monitor state) — no framework, no build-time modules beyond the `esptool-js` import.

- **Connect**: `navigator.serial.requestPort()`, then construct an `esptool-js` `Transport`/`ESPLoader` and call `.main()` to detect the chip. Retries a few times on "port busy" errors — on Linux, ModemManager probes new USB-serial devices for ~15s and holds the port.
- **Flash**: fetches `./firmware/status-light.bin`, validates the first byte is `0xe9` (ESP image magic) before doing anything, then `esploader.writeFlash(...)` with progress reporting.
- **Provision**: after flashing, the port must be handed off from esptool-js's transport to a plain serial monitor. This handoff is the fragile part of the whole app — see below.
- Provisioning protocol: after flash + reset, the flasher sends one newline-terminated JSON line at 115200 baud and waits (10s timeout) for a `{"status":...}` JSON reply. The line carries a `mode` and the fields for that mode:
  - MQTT: `{"cmd":"provision","mode":"mqtt","ssid":...,"password":...,"mqttTransport":"tcp|ws|wss","mqttHost":...,"mqttPort":1883,"mqttPath":"/mqtt","mqttUsername":...,"mqttPassword":...,"printerId":...}` — the shared broker credential (`mqttUsername`/`mqttPassword`) comes from the dashboard's **Status Light** card (admin) or `GET /api/status-light/provisioning`.
  - API: `{"cmd":"provision","mode":"api","ssid":...,"password":...,"serverUrl":"http://host:8080","pollInterval":10000,"printerId":...}`.
  The firmware also answers `ping`, `status`, and `reset` commands the same way — useful for debugging over a plain terminal.

### Known-fragile area: post-flash serial handoff

`esptool-js`'s `Transport.disconnect()` can leave the port open with locked read/write streams. Reusing that handle for the provisioning monitor reads nothing and times out. The fix (`reopenSerialPort()` in `src/main.js`) force-closes then reopens the port for fresh unlocked streams, and `startMonitor()` sends an initial `ping` to confirm the link is live *before* the user attempts provisioning — so a broken handoff surfaces immediately as "device did not answer the initial ping" instead of a confusing timeout mid-provisioning.

If touching this flow: Chrome's native serial device picker cannot be driven by automation, and an automated browser tab has no port grant — testing requires a human running Start Over → Connect → Flash and reporting the console/terminal log. Direct terminal testing (`esptool` + `pyserial` against `/dev/ttyACM0` with the device unplugged from Chrome) is useful for isolating whether a bug is in the firmware or in the browser-side handoff.

## Firmware architecture (`firmware/status-light/`)

`status-light.ino` is a single-file Arduino sketch; `config.h` holds all pins, timeouts, NVS keys, and LED colors as `#define`s — change hardware/behavior defaults there rather than inline.

- **Config storage**: WiFi/server/printer config lives in NVS (`Preferences`, namespace `statuslight`). Missing config → `STATE_PROVISIONING`.
- **State machine** (`DeviceState` in `config.h`): `PROVISIONING → WIFI_CONNECTING → [MQTT_CONNECTING] → RUNNING`, with `WIFI_FAILED` and a unified `LINK_ERROR` side state (broker dropped in MQTT mode, or `MAX_CONSECUTIVE_ERRORS` failed polls in API mode). `startLink()` branches on `cfgMode` after WiFi comes up — MQTT mode connects the broker, API mode goes straight to `RUNNING` and polls. `updateLED()` maps state (and, in `RUNNING`, `PrinterStatus`) to a non-blocking LED animation (`ledSolid`/`ledBreathing`/`ledBlink`/`ledTripleBlink`/`ledRainbow`), all driven off `millis()` so `loop()` never blocks.
- **Serial protocol**: newline-delimited JSON commands (`provision`, `status`, `reset`, `ping`) handled in `processSerialCommand()`, mirroring what `src/main.js` sends. `provision` carries a `mode` plus that mode's fields; both sides must stay in sync if the protocol changes.
- **MQTT mode**: connects to the dashboard's embedded broker (`server/statusLightBroker.js`) via the ESP-IDF `esp_mqtt_client` (bundled with the Arduino-ESP32 core — no extra `lib_deps`), **subscribes** to `printfarm/printers/<printerId>/status` (retained, plain string `idle|printing|paused|error|offline`), and publishes `online` to `printfarm/lights/<printerId>/availability` on connect (retained; `offline` is the MQTT LWT). `wss` validates against the built-in public-CA bundle (`esp_crt_bundle`); a lost connection trips `STATE_LINK_ERROR` while esp-mqtt auto-reconnects.
- **API mode**: polls `GET <serverUrl>/api/status-light/printers/<printerId>` (`HTTPClient` + `WiFiClientSecure`; `https` uses `setInsecure()` TOFU since the body is a non-secret status string) every `pollInterval` ms, mapping the returned `status` to LED color/animation. `MAX_CONSECUTIVE_ERRORS` failed polls trips `STATE_LINK_ERROR`.
- Hardware default: discrete common-cathode RGB LED, driven with PWM (`ledcAttach`/`ledcWrite`) on GPIO3 (R), GPIO4 (G), GPIO5 (B) (`LED_PIN_R`/`LED_PIN_G`/`LED_PIN_B`); common leg to GND. GPIO2/8/9 are avoided as they're ESP32-C3 strapping pins.
