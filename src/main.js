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
  try {
    await port.open({ baudRate: PROVISION_BAUD });
  } catch (err) {
    // Already open (some stacks keep it open across reset) — carry on.
    if (!String(err.message).includes('already open')) {
      log(`Serial monitor error: ${err.message}`, 'error');
      return;
    }
  }
  monitorRunning = true;
  lineBuffer = '';
  readLoop();
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

// ─── Step 3: Configure / provision ────────────────────────────────────────────

async function provision(event) {
  event.preventDefault();
  setBusy(el.btnProvision, true);
  el.terminalContainer.open = true;
  try {
    const seconds = Number($('inputPollInterval').value) || 10;
    const payload = {
      cmd: 'provision',
      ssid: $('inputSSID').value.trim(),
      password: el.inputPassword.value,
      serverUrl: $('inputServerUrl').value.trim(),
      printerId: $('inputPrinterId').value.trim(),
      pollInterval: Math.round(seconds * 1000),
    };

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
  $('inputPollInterval').value = '10';
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

  // If the user unplugs the device, reset back to a clean state.
  navigator.serial.addEventListener('disconnect', (e) => {
    if (port && e.target === port) {
      log('Device disconnected.', 'warning');
    }
  });

  setStep(1);
}

init();
