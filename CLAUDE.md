# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A two-part project for an ESP32-C3-based 3D-printer-farm status light:

1. **Web flasher** (`index.html`, `src/main.js`, `src/style.css`) — a static Vite/vanilla-JS site. Uses `esptool-js` over the Web Serial API to flash a pre-built firmware binary, then opens a plain serial link to provision WiFi + server config via JSON.
2. **ESP32-C3 firmware** (`firmware/status-light/status-light.ino`, `config.h`) — an Arduino sketch that connects to WiFi, polls a print-farm status endpoint, and drives a WS2812B LED to reflect printer state.

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

1. Copy `firmware/status-light/status-light.ino` into a PlatformIO project as `src/main.cpp`, adding `#include <Arduino.h>` and forward prototypes for `processSerialCommand`/`clearConfig` (the `.ino` relies on the Arduino builder's automatic prototype generation, which PlatformIO's C++ build does not do).
2. `platformio.ini`: `board = esp32-c3-devkitm-1`, `framework = arduino`, `lib_deps` = `adafruit/Adafruit NeoPixel` + `bblanchon/ArduinoJson@^7`. **Must** set `build_flags = -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1` — most ESP32-C3 "Super Mini" boards use the chip's native USB Serial/JTAG (enumerates as `303a:1001`, e.g. `/dev/ttyACM0`). Without `CDC_ON_BOOT`, the firmware's `Serial` goes to UART0 instead and the browser never sees any output.
3. `pio run`, then merge bootloader + partition table + `boot_app0` + app into a single image at offset `0x0` (the manifest and `src/main.js` both flash one binary at `0x0`):
   ```
   esptool.py --chip esp32c3 merge_bin -o status-light.bin \
     --flash_mode dio --flash_freq 80m --flash_size 4MB \
     0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
   ```
   A bare app `.bin` alone (offset `0x10000`) will flash but won't boot from `0x0`.
4. Copy the merged `status-light.bin` into `public/firmware/`.

`public/firmware/manifest.json` declares the chip family (`ESP32-C3`) and the single part/offset the flasher writes.

## Web flasher architecture (`src/main.js`)

Single state machine driving four UI panels/steps: **connect → flash → provision → done**. All state is module-level (`port`, `transport`, `esploader`, monitor state) — no framework, no build-time modules beyond the `esptool-js` import.

- **Connect**: `navigator.serial.requestPort()`, then construct an `esptool-js` `Transport`/`ESPLoader` and call `.main()` to detect the chip. Retries a few times on "port busy" errors — on Linux, ModemManager probes new USB-serial devices for ~15s and holds the port.
- **Flash**: fetches `./firmware/status-light.bin`, validates the first byte is `0xe9` (ESP image magic) before doing anything, then `esploader.writeFlash(...)` with progress reporting.
- **Provision**: after flashing, the port must be handed off from esptool-js's transport to a plain serial monitor. This handoff is the fragile part of the whole app — see below.
- Provisioning protocol: after flash + reset, the flasher sends one newline-terminated JSON line `{"cmd":"provision","ssid":...,"password":...,"serverUrl":...,"printerId":...,"pollInterval":...}` at 115200 baud and waits (10s timeout) for a `{"status":...}` JSON reply. The firmware also answers `ping`, `status`, and `reset` commands the same way — useful for debugging over a plain terminal.

### Known-fragile area: post-flash serial handoff

`esptool-js`'s `Transport.disconnect()` can leave the port open with locked read/write streams. Reusing that handle for the provisioning monitor reads nothing and times out. The fix (`reopenSerialPort()` in `src/main.js`) force-closes then reopens the port for fresh unlocked streams, and `startMonitor()` sends an initial `ping` to confirm the link is live *before* the user attempts provisioning — so a broken handoff surfaces immediately as "device did not answer the initial ping" instead of a confusing timeout mid-provisioning.

If touching this flow: Chrome's native serial device picker cannot be driven by automation, and an automated browser tab has no port grant — testing requires a human running Start Over → Connect → Flash and reporting the console/terminal log. Direct terminal testing (`esptool` + `pyserial` against `/dev/ttyACM0` with the device unplugged from Chrome) is useful for isolating whether a bug is in the firmware or in the browser-side handoff.

## Firmware architecture (`firmware/status-light/`)

`status-light.ino` is a single-file Arduino sketch; `config.h` holds all pins, timeouts, NVS keys, and LED colors as `#define`s — change hardware/behavior defaults there rather than inline.

- **Config storage**: WiFi/server/printer config lives in NVS (`Preferences`, namespace `statuslight`). Missing config → `STATE_PROVISIONING`.
- **State machine** (`DeviceState` in `config.h`): `PROVISIONING → WIFI_CONNECTING → RUNNING`, with `WIFI_FAILED` and `HTTP_ERROR` side states reached on timeout/repeated failure. `updateLED()` maps state (and, in `RUNNING`, `PrinterStatus`) to a non-blocking LED animation (`ledSolid`/`ledBreathing`/`ledBlink`/`ledTripleBlink`/`ledRainbow`), all driven off `millis()` so `loop()` never blocks.
- **Serial protocol**: newline-delimited JSON commands (`provision`, `status`, `reset`, `ping`) handled in `processSerialCommand()`, mirroring what `src/main.js` sends. Both sides must stay in sync if the protocol changes.
- **Polling**: once running, polls `GET <serverUrl>/api/status-light/printers/<printerId>` on `cfgPollInterval`, expects `{"status": "idle|printing|paused|error|offline"}`, and maps that to LED color/animation via the `COLOR_*` defines in `config.h`. `MAX_CONSECUTIVE_ERRORS` failed polls trips `STATE_HTTP_ERROR`.
- Hardware default: WS2812B on GPIO8 (onboard LED on most ESP32-C3 Super Mini boards), single LED (`NUM_LEDS`).
