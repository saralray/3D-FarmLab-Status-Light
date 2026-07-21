/*
 * 3D FarmLab — Status Light Web Flasher
 * =====================================
 *
 * Drives the whole UI state machine:
 *   connect → flash → provision → done
 *
 *  - Connect:   Web Serial requestPort + esptool-js chip detection
 *  - Flash:     esptool-js writeFlash of public/firmware/status-light.bin
 *  - Provision: plain 115200 serial link, sends the firmware's JSON
 *               `{"cmd":"provision",...}` line and waits for the ok reply
 *
 * The provisioning protocol is defined by firmware/status-light/status-light.ino.
 */

import { ESPLoader, Transport } from 'esptool-js';

// Merged firmware image served from public/. Offset 0 per public/firmware/manifest.json.
const FIRMWARE_URL = './firmware/status-light.bin';
const ESP_IMAGE_MAGIC = 0xe9;          // First byte of every ESP flash image
const FLASH_BAUD = 460800;             // Fast but reliable on common USB-UART bridges
const ROM_BAUD = 115200;               // Initial ROM bootloader speed
const PROVISION_BAUD = 115200;         // Firmware serial speed (SERIAL_BAUD in config.h)
const PROVISION_TIMEOUT_MS = 10000;
const SAVED_CONFIG_KEY = 'farmlab-flasher-config'; // localStorage key for remembered WiFi/server
const DEVICES_TIMEOUT_MS = 8000;

// ─── DOM ─────────────────────────────────────────────────────────────────────

const $ = (id) => document.getElementById(id);

const el = {
  browserSupport: $('browserSupport'),
  badgeText: document.querySelector('#browserSupport .badge-text'),
  steps: Array.from(document.querySelectorAll('.step')),
  panels: {
    1: $('panelConnect'),
    2: $('panelFlash'),
    3: $('panelConfig'),
    4: $('panelDone'),
  },
  btnConnect: $('btnConnect'),
  chipInfo: $('chipInfo'),
  chipName: $('chipName'),
  btnFlash: $('btnFlash'),
  progressContainer: $('progressContainer'),
  progressText: $('progressText'),
  progressFill: $('progressFill'),
  configForm: $('configForm'),
  btnProvision: $('btnProvision'),
  btnTogglePass: $('btnTogglePass'),
  inputPassword: $('inputPassword'),
  eyeIcon: $('eyeIcon'),
  inputRemember: $('inputRemember'),
  inputServerUrl: $('inputServerUrl'),
  inputMqttTransport: $('inputMqttTransport'),
  inputMqttHost: $('inputMqttHost'),
  inputMqttPort: $('inputMqttPort'),
  inputMqttUser: $('inputMqttUser'),
  inputMqttPass: $('inputMqttPass'),
  inputPrinterId: $('inputPrinterId'),
  printerSelect: $('printerSelect'),
  btnLoadDevices: $('btnLoadDevices'),
  btnStartOver: $('btnStartOver'),
  terminalContainer: $('terminalContainer'),
  terminal: $('terminal'),
  unsupportedOverlay: $('unsupportedOverlay'),
  ledDome: document.querySelector('.led-dome'),
};

// ─── Runtime state ────────────────────────────────────────────────────────────

let port = null;           // Raw Web Serial port (kept across the whole flow)
let transport = null;      // esptool-js Transport wrapper
let esploader = null;      // esptool-js loader

let monitorReader = null;  // Active reader on the plain provisioning link
let monitorRunning = false;
let lineBuffer = '';       // Accumulates partial serial lines
let awaitingReply = null;  // { resolve, reject } while waiting for a JSON reply

// ─── Terminal ─────────────────────────────────────────────────────────────────

function log(message, kind = 'info') {
  const line = document.createElement('div');
  line.className = `log-${kind}`;
  line.textContent = message;
  el.terminal.appendChild(line);
  el.terminal.scrollTop = el.terminal.scrollHeight;
}

// esptool-js writes flashing progress through this interface.
const espLoaderTerminal = {
  clean() { el.terminal.innerHTML = ''; },
  writeLine(data) { log(data, 'data'); },
  write(data) {
    // esptool streams partial lines (e.g. dots); coalesce into the last node.
    const last = el.terminal.lastElementChild;
    if (last && last.classList.contains('log-data')) {
      last.textContent += data;
    } else {
      log(data, 'data');
    }
    el.terminal.scrollTop = el.terminal.scrollHeight;
  },
};

// ─── UI helpers ───────────────────────────────────────────────────────────────

function setStep(n) {
  el.steps.forEach((step) => {
    const s = Number(step.dataset.step);
    step.classList.toggle('active', s === n);
    step.classList.toggle('done', s < n);
  });
  Object.entries(el.panels).forEach(([s, panel]) => {
    panel.classList.toggle('hidden', Number(s) !== n);
  });
}

function setBusy(btn, busy) {
  btn.classList.toggle('loading', busy);
  btn.disabled = busy;
}

function setLed(color) {
  if (el.ledDome) el.ledDome.style.background = color;
}

function setProgress(fraction) {
  const pct = Math.max(0, Math.min(100, Math.round(fraction * 100)));
  el.progressText.textContent = `${pct}%`;
  el.progressFill.style.width = `${pct}%`;
}

// ─── Remembered WiFi / server config ──────────────────────────────────────────

function loadSavedConfig() {
  try {
    const raw = localStorage.getItem(SAVED_CONFIG_KEY);
    return raw ? JSON.parse(raw) : null;
  } catch {
    return null;
  }
}

function saveConfig({ ssid, password, serverUrl }) {
  try {
    localStorage.setItem(SAVED_CONFIG_KEY, JSON.stringify({ ssid, password, serverUrl }));
  } catch { /* storage unavailable — best effort */ }
}

function clearSavedConfig() {
  try { localStorage.removeItem(SAVED_CONFIG_KEY); } catch { /* noop */ }
}

// Fill the form from whatever was remembered last time, then try to populate
// the device dropdown if we already know the server URL.
function prefillSavedConfig() {
  const saved = loadSavedConfig();
  if (!saved) return;
  if (saved.ssid) $('inputSSID').value = saved.ssid;
  if (saved.password) el.inputPassword.value = saved.password;
  if (saved.serverUrl) {
    el.inputServerUrl.value = saved.serverUrl;
    syncMqttHostFromServerUrl();
    loadDevices();
  }
}

// The MQTT broker runs on the dashboard host, so derive the broker host from the
// entered server URL (the device's WiFi must reach it). Only auto-fill when the
// field is empty so a manually-entered host is never clobbered.
function syncMqttHostFromServerUrl() {
  if (el.inputMqttHost.value.trim()) return;
  const raw = el.inputServerUrl.value.trim();
  if (!raw) return;
  try {
    el.inputMqttHost.value = new URL(raw).hostname;
  } catch {
    // Ignore an incomplete/invalid URL — the user can type the host manually.
  }
}

// ─── Browser support ──────────────────────────────────────────────────────────

function checkSupport() {
  if ('serial' in navigator) {
    el.browserSupport.classList.add('supported');
    el.badgeText.textContent = 'Web Serial ready';
    return true;
  }
  el.browserSupport.classList.add('unsupported');
  el.badgeText.textContent = 'Not supported';
  el.unsupportedOverlay.classList.remove('hidden');
  el.btnConnect.disabled = true;
  return false;
}

// ─── Step 1: Connect ──────────────────────────────────────────────────────────

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const isPortBusy = (err) => /failed to open|already open|access denied|busy/i.test(String(err && err.message));

async function connect() {
  setBusy(el.btnConnect, true);
  el.terminalContainer.open = true;
  try {
    log('Requesting serial port…');
    port = await navigator.serial.requestPort();

    // The port can be momentarily held open by the OS (on Linux, ModemManager
    // probes new USB-serial devices for ~15s). Retry the open a few times.
    const MAX_ATTEMPTS = 4;
    let chip = null;
    for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
      transport = new Transport(port, false);
      esploader = new ESPLoader({
        transport,
        baudrate: FLASH_BAUD,
        romBaudrate: ROM_BAUD,
        terminal: espLoaderTerminal,
      });
      try {
        log(attempt === 1 ? 'Connecting to bootloader…' : `Port busy — retry ${attempt}/${MAX_ATTEMPTS}…`);
        chip = await esploader.main();
        break;
      } catch (err) {
        await cleanupTransport();
        if (isPortBusy(err) && attempt < MAX_ATTEMPTS) {
          await sleep(4000);
          continue;
        }
        throw err;
      }
    }

    el.chipName.textContent = chip;
    el.chipInfo.style.display = '';
    log(`Detected: ${chip}`, 'success');
    setLed('radial-gradient(circle at 40% 35%, #7fe0ff, #3b9eff 60%, #1c66c9)');

    setStep(2);
  } catch (err) {
    log(`Connect failed: ${err.message || err}`, 'error');
    if (isPortBusy(err)) {
      log('The serial port is held by another program. Close any Arduino/serial monitor or other tab using it.', 'warning');
      log('On Linux, ModemManager grabs the port — run: sudo systemctl stop ModemManager (then unplug/replug and retry).', 'warning');
    }
    await cleanupTransport();
    port = null;
  } finally {
    setBusy(el.btnConnect, false);
  }
}

// ─── Step 2: Flash ────────────────────────────────────────────────────────────

async function fetchFirmware() {
  log(`Fetching firmware image (${FIRMWARE_URL})…`);
  let res;
  try {
    res = await fetch(FIRMWARE_URL, { cache: 'no-store' });
  } catch (err) {
    throw new Error(`Could not download firmware: ${err.message}`);
  }
  if (!res.ok) {
    throw new Error(
      `Firmware image not found (HTTP ${res.status}). Build it with ` +
      `arduino-cli and place it at public/firmware/status-light.bin.`,
    );
  }
  const buf = await res.arrayBuffer();
  const bytes = new Uint8Array(buf);
  if (bytes.length === 0) {
    throw new Error('Firmware image is empty — replace the placeholder .bin with a real build.');
  }
  if (bytes[0] !== ESP_IMAGE_MAGIC) {
    throw new Error(
      `File at ${FIRMWARE_URL} is not a valid ESP firmware image ` +
      `(first byte 0x${bytes[0].toString(16)}, expected 0xe9). ` +
      `Rebuild the firmware and commit the real binary.`,
    );
  }
  log(`Firmware image: ${bytes.length.toLocaleString()} bytes`, 'success');

  // esptool-js wants the image as a binary (Latin-1) string.
  let binary = '';
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode.apply(null, bytes.subarray(i, i + chunk));
  }
  return binary;
}

async function flash() {
  setBusy(el.btnFlash, true);
  el.terminalContainer.open = true;
  try {
    const data = await fetchFirmware();

    el.progressContainer.style.display = '';
    setProgress(0);
    log('Writing firmware — keep the board plugged in…', 'warning');

    await esploader.writeFlash({
      fileArray: [{ data, address: 0 }],
      flashSize: 'keep',
      flashMode: 'keep',
      flashFreq: 'keep',
      eraseAll: false,
      compress: true,
      reportProgress: (_fileIndex, written, total) => setProgress(written / total),
    });

    setProgress(1);
    log('Flash complete. Resetting device…', 'success');
    await esploader.after();          // Hard-reset into the freshly flashed app

    // Hand the port off from esptool to the plain provisioning monitor.
    await cleanupTransport();
    await startMonitor();

    setLed('radial-gradient(circle at 40% 35%, #b9ffd0, #34e27a 60%, #12a552)');
    setStep(3);
  } catch (err) {
    log(`Flash failed: ${err.message || err}`, 'error');
  } finally {
    setBusy(el.btnFlash, false);
  }
}

// ─── Provisioning serial link ─────────────────────────────────────────────────

async function startMonitor() {
  if (!port) return;

  // esptool-js drove this same port during flashing; hand it back to the plain
  // provisioning link with a clean close→open so we get fresh, unlocked read and
  // write streams (a reused/half-open handle reads nothing and times out).
  const reopened = await reopenSerialPort();
  if (!reopened) {
    log('Could not reopen the serial port after flashing.', 'error');
    log('Click "Start over" and reconnect — the flashed device is ready to configure.', 'warning');
    return;
  }
  port = reopened;

  monitorRunning = true;
  lineBuffer = '';
  readLoop();
  log('Serial monitor connected.', 'success');

  // The firmware boots ~1s after the reset; give it a moment, then confirm the
  // link is actually live by pinging it. This surfaces a broken link here
  // instead of as a mysterious timeout during provisioning.
  await sleep(1200);
  try {
    const pong = await sendCommand({ cmd: 'ping' });
    if (pong && pong.status === 'ok') {
      log(`Device online — firmware ${pong.firmware || '?'}.`, 'success');
    }
  } catch {
    log('Device did not answer the initial ping — it may still be booting; provisioning will retry.', 'warning');
  }
}

// (Re)open the provisioning link on a clean handle. esptool-js's transport may
// leave the port open with locked streams, so force a close first, then reopen.
// Retries a few times while the USB stack settles after the reset.
async function reopenSerialPort() {
  // Prefer the handle we already have; fall back to whatever the browser lists.
  let target = port;
  try {
    const granted = await navigator.serial.getPorts();
    if (!target && granted.length) target = granted[0];
  } catch { /* getPorts unavailable — stick with the existing handle */ }
  if (!target) return null;

  for (let attempt = 0; attempt < 10; attempt++) {
    // Force a clean slate — a port left open by esptool won't reopen and its
    // streams stay locked, so close it (ignoring "already closed") first.
    try { await target.close(); } catch { /* wasn't open */ }
    try {
      await target.open({ baudRate: PROVISION_BAUD });
      await releaseBootSignals(target);
      return target;
    } catch { /* not ready yet — wait and retry */ }
    await sleep(400);
  }
  return null;
}

// Match a plain serial terminal (DTR off, RTS off) after opening. Harmless on a
// native-USB ESP32-C3 (it answers in any signal state) but keeps us aligned with
// a normal monitor and avoids poking the boot/reset strapping on UART-bridge boards.
async function releaseBootSignals(p) {
  if (typeof p.setSignals !== 'function') return;
  try {
    await p.setSignals({ dataTerminalReady: false, requestToSend: false });
  } catch { /* signals unsupported on this platform — best effort */ }
}

async function readLoop() {
  const decoder = new TextDecoder();
  while (monitorRunning && port && port.readable) {
    monitorReader = port.readable.getReader();
    try {
      while (true) {
        const { value, done } = await monitorReader.read();
        if (done) break;
        if (value) handleIncoming(decoder.decode(value, { stream: true }));
      }
    } catch (err) {
      if (monitorRunning) log(`Serial read error: ${err.message}`, 'error');
    } finally {
      try { monitorReader.releaseLock(); } catch { /* noop */ }
      monitorReader = null;
    }
  }
}

function handleIncoming(text) {
  lineBuffer += text;
  let idx;
  while ((idx = lineBuffer.search(/[\r\n]/)) >= 0) {
    const line = lineBuffer.slice(0, idx).trim();
    lineBuffer = lineBuffer.slice(idx + 1);
    if (line) handleLine(line);
  }
}

function handleLine(line) {
  log(line, 'data');
  if (!awaitingReply) return;
  // The firmware answers commands with a JSON object carrying a "status" field.
  if (line.startsWith('{') && line.includes('"status"')) {
    try {
      const obj = JSON.parse(line);
      const pending = awaitingReply;
      awaitingReply = null;
      pending.resolve(obj);
    } catch { /* not the reply we're waiting for */ }
  }
}

async function writeSerial(str) {
  const writer = port.writable.getWriter();
  try {
    await writer.write(new TextEncoder().encode(str));
  } finally {
    writer.releaseLock();
  }
}

function sendCommand(payload) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      awaitingReply = null;
      reject(new Error('Timed out waiting for the device to reply.'));
    }, PROVISION_TIMEOUT_MS);

    awaitingReply = {
      resolve: (obj) => { clearTimeout(timer); resolve(obj); },
      reject: (e) => { clearTimeout(timer); reject(e); },
    };

    writeSerial(JSON.stringify(payload) + '\n').catch((e) => {
      clearTimeout(timer);
      awaitingReply = null;
      reject(e);
    });
  });
}

// ─── Device picker (GET <serverUrl>/api/status-light/devices) ────────────────
// Public, unauthenticated "frontend mirror" of the devices list — mirrors the
// path shape of the firmware's own polling endpoint (/api/status-light/printers/:id).
// The cookie-session dashboard path (/status-light/devices, no /api/ prefix)
// requires a logged-in session and isn't reachable from this static flasher.

async function loadDevices() {
  const serverUrl = el.inputServerUrl.value.trim().replace(/\/+$/, '');
  if (!serverUrl) {
    log('Enter a server URL first to load the device list.', 'warning');
    return;
  }

  setBusy(el.btnLoadDevices, true);
  try {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), DEVICES_TIMEOUT_MS);
    let res;
    try {
      res = await fetch(`${serverUrl}/api/status-light/devices`, { signal: controller.signal });
    } finally {
      clearTimeout(timer);
    }
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();
    const devices = Array.isArray(data.devices) ? data.devices : [];
    populateDeviceSelect(devices);
    log(`Loaded ${devices.length} device${devices.length === 1 ? '' : 's'} from server.`, 'success');
  } catch (err) {
    populateDeviceSelect([]);
    log(`Could not load device list: ${err.message || err}`, 'warning');
    if (err.name === 'TypeError') {
      // Chrome/Firefox both throw a generic TypeError for both a CORS block and
      // a DNS/network failure — "Failed to fetch" gives no further detail. Since
      // the same request works fine outside a browser (e.g. curl), CORS is by
      // far the more likely cause when the server URL itself is reachable.
      log(
        'This usually means the server isn\'t sending an Access-Control-Allow-Origin ' +
        'header for /api/status-light/devices — browsers block cross-origin reads ' +
        'without it, even though the endpoint works fine outside a browser.',
        'warning',
      );
    }
    log('You can still type the printer ID manually below.', 'warning');
  } finally {
    setBusy(el.btnLoadDevices, false);
  }
}

function populateDeviceSelect(devices) {
  const select = el.printerSelect;
  select.innerHTML = '';

  const placeholder = document.createElement('option');
  placeholder.value = '';
  placeholder.textContent = devices.length ? 'Select a printer…' : 'No devices found';
  select.appendChild(placeholder);

  devices
    .slice()
    .sort((a, b) => String(a.printerId).localeCompare(String(b.printerId)))
    .forEach((d) => {
      const opt = document.createElement('option');
      opt.value = d.printerId;
      opt.textContent = `${d.printerId} — ${d.connected ? 'active' : 'offline'}`;
      select.appendChild(opt);
    });

  select.disabled = devices.length === 0;
}

function onPrinterSelectChange() {
  if (el.printerSelect.value) {
    el.inputPrinterId.value = el.printerSelect.value;
  }
}

// ─── Step 3: Configure / provision ────────────────────────────────────────────

async function provision(event) {
  event.preventDefault();
  setBusy(el.btnProvision, true);
  el.terminalContainer.open = true;
  try {
    const transport = el.inputMqttTransport.value || 'tcp';
    const payload = {
      cmd: 'provision',
      ssid: $('inputSSID').value.trim(),
      password: el.inputPassword.value,
      // The device subscribes to the dashboard's MQTT broker for pushed status
      // (server/statusLightBroker.js); no HTTP polling.
      mqttTransport: transport,
      mqttHost: el.inputMqttHost.value.trim(),
      mqttPort: Number(el.inputMqttPort.value) || 1883,
      mqttPath: '/mqtt',
      mqttUsername: el.inputMqttUser.value.trim(),
      mqttPassword: el.inputMqttPass.value,
      printerId: el.inputPrinterId.value.trim(),
    };

    if (el.inputRemember.checked) {
      saveConfig({ ssid: payload.ssid, password: payload.password, serverUrl: el.inputServerUrl.value.trim() });
    } else {
      clearSavedConfig();
    }

    // Don't print the WiFi password to the console.
    log(`Sending config for "${payload.printerId}" on "${payload.ssid}"…`);
    const reply = await sendCommand(payload);

    if (reply.status === 'ok') {
      log(`Device: ${reply.msg || 'Config saved.'}`, 'success');
      setLed('radial-gradient(circle at 40% 35%, #b9ffd0, #34e27a 60%, #12a552)');
      setStep(4);
    } else {
      log(`Device rejected config: ${reply.msg || 'unknown error'}`, 'error');
    }
  } catch (err) {
    log(`Provisioning failed: ${err.message || err}`, 'error');
  } finally {
    setBusy(el.btnProvision, false);
  }
}

// ─── Teardown / start over ────────────────────────────────────────────────────

async function cleanupTransport() {
  if (transport) {
    try { await transport.disconnect(); } catch { /* noop */ }
    transport = null;
    esploader = null;
  }
}

async function stopMonitor() {
  monitorRunning = false;
  if (monitorReader) {
    try { await monitorReader.cancel(); } catch { /* noop */ }
  }
  if (port) {
    try { await port.close(); } catch { /* noop */ }
  }
}

async function startOver() {
  await cleanupTransport();
  await stopMonitor();
  port = null;
  awaitingReply = null;
  lineBuffer = '';

  el.chipInfo.style.display = 'none';
  el.progressContainer.style.display = 'none';
  setProgress(0);
  el.configForm.reset();
  el.inputMqttPort.value = '1883';
  el.inputMqttUser.value = 'statuslight';
  populateDeviceSelect([]);
  prefillSavedConfig();
  setLed('');
  log('Ready for the next device.');
  setStep(1);
}

// ─── Wire-up ──────────────────────────────────────────────────────────────────

function togglePassword() {
  const showing = el.inputPassword.type === 'text';
  el.inputPassword.type = showing ? 'password' : 'text';
  el.btnTogglePass.setAttribute('aria-label', showing ? 'Show password' : 'Hide password');
  el.btnTogglePass.title = showing ? 'Show password' : 'Hide password';
}

function init() {
  if (!checkSupport()) return;

  el.btnConnect.addEventListener('click', connect);
  el.btnFlash.addEventListener('click', flash);
  el.configForm.addEventListener('submit', provision);
  el.btnStartOver.addEventListener('click', startOver);
  el.btnTogglePass.addEventListener('click', togglePassword);
  el.btnLoadDevices.addEventListener('click', loadDevices);
  el.printerSelect.addEventListener('change', onPrinterSelectChange);
  el.inputServerUrl.addEventListener('change', () => {
    syncMqttHostFromServerUrl();
    loadDevices();
  });

  // If the user unplugs the device, reset back to a clean state.
  navigator.serial.addEventListener('disconnect', (e) => {
    if (port && e.target === port) {
      log('Device disconnected.', 'warning');
    }
  });

  prefillSavedConfig();
  setStep(1);
}

init();
