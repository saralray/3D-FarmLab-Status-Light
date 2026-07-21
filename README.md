ESP32-C3 Status Light — Web Flasher + Firmware

Project Overview

This repository implements a complete web-based flasher and firmware for an ESP32-C3-based status light that subscribes to a 3D printer farm dashboard's embedded MQTT broker and drives an RGB status LED (discrete common-cathode LED). The goal is a simple, secure, and friendly experience where users can flash a pre-built firmware binary from a Vercel-hosted static web app and provision WiFi + broker settings over serial immediately after flashing. (The 3D-FarmLab dashboard also has its own in-browser Status Light flash card — this standalone flasher is an alternative.)

High-level deliverables

- Web Flasher App (Vite, Vanilla JS) — Hosted as a static site (deployable to Vercel). Uses Web Serial API + esptool-js to flash a pre-built firmware binary and then sends a single-line JSON provisioning payload over serial.
- ESP32-C3 Arduino Firmware — C++ Arduino sketch that connects to WiFi, subscribes to the dashboard's MQTT broker for pushed printer status, and maps status → RGB LED animation/color.
- Pre-built firmware binary — committed (placeholder) binary at public/firmware/status-light.bin so the web flasher can serve a ready-to-flash image.

Why this split?

- End users get a browser-based experience with no local tooling beyond a supported browser and USB connection to the device.
- Developers can iterate on the Arduino firmware source, compile their own binary, and replace the committed placeholder.

Repository layout (planned)

- package.json — root Vite + dependency metadata for the web flasher
- vite.config.js — build config
- vercel.json — Vercel deployment config
- index.html — flasher UI
- src/main.js — web flasher logic (Web Serial, esptool-js workflows, UI state)
- src/style.css — UI styles
- public/firmware/manifest.json — Web Tools / manifest for the binary
- public/firmware/status-light.bin — pre-built firmware placeholder
- firmware/status-light/ — Arduino sketch and helper files
  - status-light.ino
  - config.h
  - README.md (wiring + build instructions)
- README.md — (this file)
- .gitignore

Web Flasher (User-facing app)

Tech stack

- Vite static site
- Vanilla JavaScript
- esptool-js (to run ESP flashing in-browser via WebUSB / Web Serial)
- Web Serial API for post-flash provisioning

Key features

- Connect to an ESP32-C3 device using the Web Serial API. The UI will filter by common USB vendor/device identifiers to help the browser suggestion list but allows manual device selection as well.
- One-click flash using esptool-js: erase, write, verify (progress + terminal log shown to the user).
- After a successful flash, open a serial connection at 115200 baud and send a single JSON provisioning line with WiFi + server config.
- Real-time progress bar + terminal log for visibility.

Files (planned)

- index.html — minimal single-page UI. Inputs for WiFi SSID/password, server URL (for the device picker), MQTT broker host/port/transport/credential, printer ID, and Flash button.
- src/main.js — implements state machine: idle → connecting → flashing → provisioning → done. Handles Web Serial device selection, esptool-js flash sequence, and writes provisioning JSON after flash.
- src/style.css — modern dark theme, responsive layout, and animations.

Local dev

1. Install dependencies

   npm install

2. Start dev server

   npm run dev

3. Build for production

   npm run build

4. Preview production build locally

   npm run preview

Vercel deployment

- vercel.json sets the build output directory (dist) and appropriate security headers.
- From the GitHub repo, connect the project in the Vercel dashboard and set the build command to npm run build and output directory to dist.

Security & Browser notes

- The Web Serial API requires HTTPS and user gesture device selection. Vercel-hosted site and local dev with a secure context (or using a browser flag for local testing) are required.
- esptool-js may require WebUSB or Web Serial depending on the build and runtime environment. This project targets Web Serial for maximum browser compatibility.
- The provisioning message contains a WiFi password in cleartext sent over USB serial to the device — the device must be trusted. The host site does not persist secrets.

ESP32-C3 Arduino Firmware

Purpose

- Connect to configured WiFi and subscribe to the print farm dashboard's embedded MQTT broker to get pushed printer status for a configured printerId.
- Map status to an RGB LED animation (colors + breathing/pulse effects) on 3 pins.
- Support serial provisioning: listen on 115200 baud for a single JSON line with "cmd":"provision" and config fields to write to non-volatile storage.

Main behavior

- On boot, read config from NVS namespace (ssid, password, mqttTransport, mqttHost, mqttPort, mqttPath, mqttUsername, mqttPassword, printerId). If missing, blink/pulse LED to indicate unconfigured state and wait for provisioning.
- Attempt to connect to WiFi with retry logic. During WiFi/broker connect, LED shows cyan blink.
- Once connected, subscribe to printfarm/printers/<printerId>/status (retained, plain string). The broker pushes the status; the device does not poll. Publishes online → printfarm/lights/<printerId>/availability on connect (retained; offline is the MQTT LWT).

  status ∈ idle|printing|paused|error|offline

- Map status to LED:
  - idle → Green (solid)
  - printing → Blue (breathing)
  - paused → Yellow (solid)
  - error → Red (fast pulse)
  - offline → Dim white (slow pulse)
  - WiFi/MQTT connecting → Cyan (fast pulse/blink)
  - Broker lost → Red triple blink

- On receiving serial provisioning JSON line:

  {"cmd":"provision","ssid":"...","password":"...","mqttTransport":"tcp","mqttHost":"...","mqttPort":1883,"mqttUsername":"statuslight","mqttPassword":"...","printerId":"..."}

  Validate, write to NVS, respond with {"status":"ok","msg":"Config saved. Rebooting..."} and reboot.

Files (planned)

- firmware/status-light/status-light.ino — main Arduino sketch implementing the above logic using Arduino-ESP32 libraries and a small LED animation helper.
- firmware/status-light/config.h — default constants, NVS namespace, pin numbers, default MQTT port/keepalive.
- firmware/status-light/README.md — wiring diagram, build and upload instructions with arduino-cli, serial provisioning protocol, LED mapping.

Building the firmware (developer)

Requirements

- Arduino CLI or Arduino IDE with esp32 board support installed (esp32:esp32:esp32c3)
- Platform package: esp32 by Espressif (supporting esp32c3)

Using arduino-cli

1. Install board package (if not already):

   arduino-cli core update-index
   arduino-cli core install esp32:esp32

2. Compile:

   arduino-cli compile --fqbn esp32:esp32:esp32c3 firmware/status-light

3. To create a binary for distribution, use the output artifact from the compile step or the Arduino IDE export compiled binary feature.

Pre-built binary

- public/firmware/status-light.bin is included as a placeholder binary so the web flasher UI can serve a ready-to-flash image.
- Recommended: Developers should compile a fresh binary from firmware/status-light and replace public/firmware/status-light.bin before publishing a production flasher.

Serial provisioning protocol (post-flash)

- After flashing completes, the web flasher opens a serial connection at 115200 baud and sends a single JSON line (newline-terminated) with the provisioning command:

  {"cmd":"provision","ssid":"MyNetwork","password":"secret123","mqttTransport":"tcp","mqttHost":"10.0.0.5","mqttPort":1883,"mqttUsername":"statuslight","mqttPassword":"<shared broker credential>","printerId":"printer-01"}

- Firmware responds with a JSON status line, e.g.:

  {"status":"ok","msg":"Config saved. Rebooting..."}

- Firmware then reboots and begins operation.

Wiring

- Common-cathode RGB LED: R leg → GPIO 3, G leg → GPIO 4, B leg → GPIO 5 (each through a ~220–330Ω current-limiting resistor), common (cathode) leg → GND. GPIO2/8/9 are avoided as they're ESP32-C3 strapping pins.
- If your board's LED pins differ, update firmware/status-light/config.h (`LED_PIN_R`/`LED_PIN_G`/`LED_PIN_B`) before compiling.

Security & Safety Notes

- The web flasher sends WiFi passwords over USB serial — treat the client machine and device as trusted.
- The hosted web flasher must be served over HTTPS to access Web Serial. Vercel deployment provides HTTPS by default.
- Do not publish pre-built binaries signed with private keys or containing secrets. The committed placeholder binary should be safe but treat binaries from unknown sources with caution.

Testing & Verification

Automated

- npm run build — verify the Vite project builds successfully and outputs to dist/.
- arduino-cli compile --fqbn esp32:esp32:esp32c3 firmware/status-light — verify the Arduino sketch compiles.

Manual

- Open the built site locally (npm run preview) or the deployed Vercel URL.
- Connect an ESP32-C3 via USB, choose device in the UI, and run through the flash flow. Observe terminal logs and progress bar.
- After flash completes, enter WiFi + server + printerId and observe provisioning response from the device.
- Confirm LED behavior after the device reboots, connects to the broker, and receives a pushed status.

Open questions / future improvements

- Printer ID field: current plan is a free-form text input (no auth). Future improvement: fetch printer list from the server URL for a dropdown — requires CORS and optional auth.
- Firmware OTA updates: consider adding a secure OTA path so devices can update without reconnecting to USB.
- Auth: for protected print farm APIs, add optional token provisioning and secure storage.
- Multiple LED support: allow configuring number of LEDs and brightness from the web flasher.

Contributing

- Feel free to open PRs for bug fixes, features, or improved UI/UX.
- To update the pre-built binary, compile with arduino-cli as described above and replace public/firmware/status-light.bin.

License

- This project is provided under the MIT license (replace or add LICENSE file as needed).

Contact / Maintainers

- Maintainer: (add contact info)


Notes

- This README continues and expands the original project spec into an actionable developer and contributor guide. If you want, the next steps can include generating the Vite project scaffold and the Arduino sketch skeleton in the repository.
