/*
 * 3D FarmLab — ESP32-C3 Printer Status Light
 * ============================================
 *
 * Firmware that polls a 3D printer farm dashboard for printer status
 * and displays it via a discrete common-cathode RGB LED, driven with PWM
 * on 3 GPIOs.
 *
 * Features:
 *   - Reads WiFi + server config from NVS (non-volatile storage)
 *   - Serial provisioning protocol (JSON commands at 115200 baud)
 *   - Non-blocking LED animations for each status
 *   - Auto-reconnect on WiFi loss
 *   - Error state after consecutive HTTP failures
 *
 * Hardware:
 *   - ESP32-C3 (Super Mini or Dev Module)
 *   - Common-cathode RGB LED: R/G/B legs on GPIO3/4/5, common leg to GND,
 *     driven as plain digital on/off outputs (no PWM)
 *
 * Libraries (install via Arduino Library Manager):
 *   - ArduinoJson (v7+)
 *
 * License: MIT
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "config.h"

// ─── Globals ────────────────────────────────────────────────────────────────

Preferences prefs;

// Config from NVS
String cfgSSID        = "";
String cfgPassword    = "";
String cfgServerUrl   = "";
String cfgPrinterId   = "";
uint32_t cfgPollInterval = DEFAULT_POLL_INTERVAL_MS;

// Runtime state
DeviceState  deviceState       = STATE_PROVISIONING;
PrinterStatus printerStatus    = STATUS_UNKNOWN;
int          consecutiveErrors = 0;
unsigned long lastPollTime     = 0;
unsigned long lastWifiAttempt  = 0;
unsigned long stateEnteredAt   = 0;

// Serial input buffer
String serialBuffer = "";

// ─── Forward Declarations ───────────────────────────────────────────────────

void loadConfig();
void saveConfig();
void handleSerial();
void connectWiFi();
void pollPrinterStatus();
void updateLED();
void setDeviceState(DeviceState newState);
PrinterStatus parseStatus(const String& s);

// LED animation helpers
void setRGB(uint8_t r, uint8_t g, uint8_t b);
void hsvToRgb(uint16_t hue, uint8_t sat, uint8_t val, uint8_t& r, uint8_t& g, uint8_t& b);
void ledSolid(uint8_t r, uint8_t g, uint8_t b);
void ledBreathing(uint8_t r, uint8_t g, uint8_t b, float freqHz);
void ledBlink(uint8_t r, uint8_t g, uint8_t b, float freqHz);
void ledTripleBlink(uint8_t r, uint8_t g, uint8_t b);
void ledRainbow();
void ledOff();

// ─── Setup ──────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(500);  // Allow serial to initialize

  // Drive the RGB legs as plain digital outputs (common-cathode: HIGH = on).
  // Matches the known-good bench test; no PWM, so no core-3.x ledc dependency.
  pinMode(LED_PIN_R, OUTPUT);
  pinMode(LED_PIN_G, OUTPUT);
  pinMode(LED_PIN_B, OUTPUT);

  // Boot self-test: prove the LED hardware + wiring work under firmware.
  // Same drive as the known-good bench sketch. If this stays dark, the
  // problem is wiring/reflash, not the status logic.
  Serial.println("[BOOT] LED self-test: R, G, B, white...");
  setRGB(255, 0, 0);   delay(400);  // Red
  setRGB(0, 255, 0);   delay(400);  // Green
  setRGB(0, 0, 255);   delay(400);  // Blue
  setRGB(255, 255, 255); delay(400);  // White
  ledOff();

  Serial.println();
  Serial.println("╔══════════════════════════════════════╗");
  Serial.println("║  3D FarmLab Status Light  v" FW_VERSION "    ║");
  Serial.println("╚══════════════════════════════════════╝");
  Serial.println();

  // Load config from NVS
  loadConfig();

  if (cfgSSID.length() == 0 || cfgServerUrl.length() == 0 || cfgPrinterId.length() == 0) {
    Serial.println("[BOOT] No configuration found. Entering provisioning mode.");
    Serial.println("[BOOT] Send JSON config via serial to provision this device.");
    Serial.println("[BOOT] Example: {\"cmd\":\"provision\",\"ssid\":\"...\",\"password\":\"...\",\"serverUrl\":\"...\",\"printerId\":\"...\",\"pollInterval\":10000}");
    setDeviceState(STATE_PROVISIONING);
  } else {
    Serial.println("[BOOT] Configuration loaded:");
    Serial.println("  SSID:         " + cfgSSID);
    Serial.println("  Server:       " + cfgServerUrl);
    Serial.println("  Printer ID:   " + cfgPrinterId);
    Serial.println("  Poll Interval: " + String(cfgPollInterval) + "ms");
    Serial.println();
    setDeviceState(STATE_WIFI_CONNECTING);
    connectWiFi();
  }
}

// ─── Main Loop ──────────────────────────────────────────────────────────────

void loop() {
  // Always listen for serial commands
  handleSerial();

  unsigned long now = millis();

  switch (deviceState) {
    case STATE_PROVISIONING:
      // Just animate rainbow and wait for serial config
      break;

    case STATE_WIFI_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[WIFI] Connected! IP: " + WiFi.localIP().toString());
        setDeviceState(STATE_RUNNING);
        consecutiveErrors = 0;
        lastPollTime = 0;  // Poll immediately
      } else if (now - stateEnteredAt > WIFI_CONNECT_TIMEOUT_MS) {
        Serial.println("[WIFI] Connection timeout. Will retry in " + String(WIFI_RETRY_INTERVAL_MS / 1000) + "s");
        setDeviceState(STATE_WIFI_FAILED);
      }
      break;

    case STATE_WIFI_FAILED:
      if (now - stateEnteredAt > WIFI_RETRY_INTERVAL_MS) {
        Serial.println("[WIFI] Retrying connection...");
        setDeviceState(STATE_WIFI_CONNECTING);
        connectWiFi();
      }
      break;

    case STATE_RUNNING:
      // Check WiFi still connected
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] Disconnected! Reconnecting...");
        setDeviceState(STATE_WIFI_CONNECTING);
        connectWiFi();
        break;
      }
      // Poll on interval
      if (now - lastPollTime >= cfgPollInterval) {
        pollPrinterStatus();
        lastPollTime = now;
      }
      break;

    case STATE_HTTP_ERROR:
      // Check WiFi
      if (WiFi.status() != WL_CONNECTED) {
        setDeviceState(STATE_WIFI_CONNECTING);
        connectWiFi();
        break;
      }
      // Keep trying on interval
      if (now - lastPollTime >= cfgPollInterval) {
        pollPrinterStatus();
        lastPollTime = now;
      }
      break;
  }

  // Update LED animation every loop iteration
  updateLED();
  delay(10);  // ~100Hz LED update rate
}

// ─── Config Management ──────────────────────────────────────────────────────

void loadConfig() {
  prefs.begin(NVS_NAMESPACE, true);  // Read-only
  cfgSSID         = prefs.getString(NVS_KEY_SSID, "");
  cfgPassword     = prefs.getString(NVS_KEY_PASSWORD, "");
  cfgServerUrl    = prefs.getString(NVS_KEY_SERVER_URL, "");
  cfgPrinterId    = prefs.getString(NVS_KEY_PRINTER_ID, "");
  cfgPollInterval = prefs.getUInt(NVS_KEY_POLL_INT, DEFAULT_POLL_INTERVAL_MS);
  prefs.end();
}

void saveConfig() {
  prefs.begin(NVS_NAMESPACE, false);  // Read-write
  prefs.putString(NVS_KEY_SSID, cfgSSID);
  prefs.putString(NVS_KEY_PASSWORD, cfgPassword);
  prefs.putString(NVS_KEY_SERVER_URL, cfgServerUrl);
  prefs.putString(NVS_KEY_PRINTER_ID, cfgPrinterId);
  prefs.putUInt(NVS_KEY_POLL_INT, cfgPollInterval);
  prefs.end();
}

void clearConfig() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
}

// ─── Serial Provisioning ────────────────────────────────────────────────────

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      serialBuffer.trim();
      if (serialBuffer.length() > 0) {
        processSerialCommand(serialBuffer);
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
      // Prevent buffer overflow
      if (serialBuffer.length() > 1024) {
        serialBuffer = "";
      }
    }
  }
}

void processSerialCommand(const String& line) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);

  if (err) {
    Serial.println("{\"status\":\"error\",\"msg\":\"Invalid JSON: " + String(err.c_str()) + "\"}");
    return;
  }

  const char* cmd = doc["cmd"] | "";

  // ── provision ─────────────────────────────────────────────────────────
  if (strcmp(cmd, "provision") == 0) {
    const char* ssid      = doc["ssid"] | "";
    const char* password  = doc["password"] | "";
    const char* serverUrl = doc["serverUrl"] | "";
    const char* printerId = doc["printerId"] | "";
    uint32_t pollInt      = doc["pollInterval"] | DEFAULT_POLL_INTERVAL_MS;

    if (strlen(ssid) == 0 || strlen(serverUrl) == 0 || strlen(printerId) == 0) {
      Serial.println("{\"status\":\"error\",\"msg\":\"Missing required fields: ssid, serverUrl, printerId\"}");
      return;
    }

    cfgSSID         = String(ssid);
    cfgPassword     = String(password);
    cfgServerUrl    = String(serverUrl);
    cfgPrinterId    = String(printerId);
    cfgPollInterval = pollInt;

    saveConfig();

    Serial.println("{\"status\":\"ok\",\"msg\":\"Config saved. Rebooting...\"}");
    Serial.flush();
    delay(1000);
    ESP.restart();
  }

  // ── status ────────────────────────────────────────────────────────────
  else if (strcmp(cmd, "status") == 0) {
    JsonDocument resp;
    resp["status"]        = "ok";
    resp["firmware"]      = FW_VERSION;
    resp["ssid"]          = cfgSSID;
    resp["serverUrl"]     = cfgServerUrl;
    resp["printerId"]     = cfgPrinterId;
    resp["pollInterval"]  = cfgPollInterval;
    resp["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
    if (WiFi.status() == WL_CONNECTED) {
      resp["ip"] = WiFi.localIP().toString();
    }
    resp["deviceState"]   = (int)deviceState;
    resp["printerStatus"] = (int)printerStatus;

    String out;
    serializeJson(resp, out);
    Serial.println(out);
  }

  // ── reset ─────────────────────────────────────────────────────────────
  else if (strcmp(cmd, "reset") == 0) {
    clearConfig();
    Serial.println("{\"status\":\"ok\",\"msg\":\"Config cleared. Rebooting...\"}");
    Serial.flush();
    delay(1000);
    ESP.restart();
  }

  // ── ping ──────────────────────────────────────────────────────────────
  else if (strcmp(cmd, "ping") == 0) {
    Serial.println("{\"status\":\"ok\",\"msg\":\"pong\",\"firmware\":\"" FW_VERSION "\"}");
  }

  // ── unknown ───────────────────────────────────────────────────────────
  else {
    Serial.println("{\"status\":\"error\",\"msg\":\"Unknown command: " + String(cmd) + "\"}");
  }
}

// ─── WiFi ───────────────────────────────────────────────────────────────────

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfgSSID.c_str(), cfgPassword.c_str());
  Serial.println("[WIFI] Connecting to: " + cfgSSID);
}

// ─── HTTP Polling ───────────────────────────────────────────────────────────

void pollPrinterStatus() {
  // Build URL: <serverUrl>/api/status-light/printers/<printerId>
  String url = cfgServerUrl;
  // Remove trailing slash if present
  if (url.endsWith("/")) {
    url = url.substring(0, url.length() - 1);
  }
  url += "/api/status-light/printers/" + cfgPrinterId;

  Serial.println("[HTTP] GET " + url);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  // HTTPClient::begin(url) alone leaves an https:// transport with no CA cert
  // and no setInsecure() — the TLS handshake just hangs until HTTP_TIMEOUT_MS
  // and fails with -11 (read timeout), never reaching the server. Route HTTPS
  // through an explicit WiFiClientSecure with cert validation skipped: the
  // response is a plain, non-secret status string, so trust-on-first-use is
  // an acceptable tradeoff for a device with no CA store to maintain.
  WiFiClientSecure secureClient;
  bool isHttps = url.startsWith("https://");
  if (isHttps) {
    secureClient.setInsecure();
    http.begin(secureClient, url);
  } else {
    http.begin(url);
  }
  http.addHeader("Accept", "application/json");

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    Serial.println("[HTTP] 200 OK: " + payload);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);

    if (!err) {
      const char* statusStr = doc["status"] | "unknown";
      printerStatus = parseStatus(String(statusStr));
      consecutiveErrors = 0;

      if (deviceState == STATE_HTTP_ERROR) {
        setDeviceState(STATE_RUNNING);
      }
    } else {
      Serial.println("[HTTP] JSON parse error: " + String(err.c_str()));
      consecutiveErrors++;
    }
  } else {
    Serial.println("[HTTP] Error: " + String(httpCode) + " " + http.errorToString(httpCode));
    consecutiveErrors++;
  }

  http.end();

  // Check if we've hit the error threshold
  if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS && deviceState != STATE_HTTP_ERROR) {
    Serial.println("[HTTP] " + String(MAX_CONSECUTIVE_ERRORS) + " consecutive errors — entering error state");
    setDeviceState(STATE_HTTP_ERROR);
  }
}

PrinterStatus parseStatus(const String& s) {
  if (s == "idle")     return STATUS_IDLE;
  if (s == "printing") return STATUS_PRINTING;
  if (s == "paused")   return STATUS_PAUSED;
  if (s == "error")    return STATUS_ERROR;
  if (s == "offline")  return STATUS_OFFLINE;
  return STATUS_UNKNOWN;
}

// ─── LED Animations ─────────────────────────────────────────────────────────

void setDeviceState(DeviceState newState) {
  deviceState = newState;
  stateEnteredAt = millis();
}

void updateLED() {
  switch (deviceState) {
    case STATE_PROVISIONING:
      ledRainbow();
      break;

    case STATE_WIFI_CONNECTING:
      ledBlink(COLOR_WIFI_R, COLOR_WIFI_G, COLOR_WIFI_B, 4.0);  // 4Hz fast blink
      break;

    case STATE_WIFI_FAILED:
      ledBreathing(COLOR_ERROR_R, COLOR_ERROR_G, COLOR_ERROR_B, 0.5);  // Slow red pulse
      break;

    case STATE_HTTP_ERROR:
      ledTripleBlink(COLOR_HTTP_ERR_R, COLOR_HTTP_ERR_G, COLOR_HTTP_ERR_B);
      break;

    case STATE_RUNNING:
      switch (printerStatus) {
        case STATUS_IDLE:
          ledSolid(COLOR_IDLE_R, COLOR_IDLE_G, COLOR_IDLE_B);
          break;
        case STATUS_PRINTING:
          ledSolid(COLOR_PRINTING_R, COLOR_PRINTING_G, COLOR_PRINTING_B);
          break;
        case STATUS_PAUSED:
          ledSolid(COLOR_PAUSED_R, COLOR_PAUSED_G, COLOR_PAUSED_B);
          break;
        case STATUS_ERROR:
          ledBlink(COLOR_ERROR_R, COLOR_ERROR_G, COLOR_ERROR_B, 2.0);  // 2Hz fast pulse
          break;
        case STATUS_OFFLINE:
          ledBreathing(COLOR_OFFLINE_R, COLOR_OFFLINE_G, COLOR_OFFLINE_B, 0.5);
          break;
        default:
          ledBreathing(COLOR_WIFI_R, COLOR_WIFI_G, COLOR_WIFI_B, 0.5);
          break;
      }
      break;
  }
}

// ── Raw output ───────────────────────────────────────────────────────────────
// Pure digital on/off per channel — no PWM, no levels. Common-cathode:
// HIGH = on, LOW = off, no inversion. Any nonzero channel value turns that
// leg fully on, exactly like the bench sketch's digitalWrite(pin, HIGH).

void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  digitalWrite(LED_PIN_R, r ? HIGH : LOW);
  digitalWrite(LED_PIN_G, g ? HIGH : LOW);
  digitalWrite(LED_PIN_B, b ? HIGH : LOW);
}

// ── Solid color ─────────────────────────────────────────────────────────────

void ledSolid(uint8_t r, uint8_t g, uint8_t b) {
  setRGB(r, g, b);
}

// ── Breathing effect (smooth sine-wave brightness) ──────────────────────────

void ledBreathing(uint8_t r, uint8_t g, uint8_t b, float freqHz) {
  float phase = (millis() % (unsigned long)(1000.0 / freqHz)) / (1000.0 / freqHz);
  float brightness = (sin(phase * 2.0 * PI) + 1.0) / 2.0;  // 0.0 → 1.0
  brightness = brightness * 0.85 + 0.15;  // Keep minimum 15% brightness

  setRGB(
    (uint8_t)(r * brightness),
    (uint8_t)(g * brightness),
    (uint8_t)(b * brightness)
  );
}

// ── Blink (on/off at frequency) ─────────────────────────────────────────────

void ledBlink(uint8_t r, uint8_t g, uint8_t b, float freqHz) {
  unsigned long period = (unsigned long)(1000.0 / freqHz);
  bool on = (millis() % period) < (period / 2);

  if (on) {
    setRGB(r, g, b);
  } else {
    setRGB(0, 0, 0);
  }
}

// ── Triple blink pattern (blink 3x, pause, repeat) ─────────────────────────

void ledTripleBlink(uint8_t r, uint8_t g, uint8_t b) {
  unsigned long cycle = millis() % 2000;  // 2-second cycle

  // Three 100ms blinks at 0, 300, 600ms, then dark for the rest
  bool on = false;
  if (cycle < 100)                      on = true;
  else if (cycle >= 300 && cycle < 400) on = true;
  else if (cycle >= 600 && cycle < 700) on = true;

  if (on) {
    setRGB(r, g, b);
  } else {
    setRGB(0, 0, 0);
  }
}

// ── Rainbow cycle (provisioning mode) ───────────────────────────────────────

void hsvToRgb(uint16_t hue, uint8_t sat, uint8_t val, uint8_t& r, uint8_t& g, uint8_t& b) {
  // hue: 0-359, sat/val: 0-255
  uint8_t region = hue / 60;
  uint8_t remainder = (hue % 60) * 255 / 60;

  uint8_t p = (val * (255 - sat)) / 255;
  uint8_t q = (val * (255 - (sat * remainder) / 255)) / 255;
  uint8_t t = (val * (255 - (sat * (255 - remainder)) / 255)) / 255;

  switch (region % 6) {
    case 0: r = val; g = t;   b = p;   break;
    case 1: r = q;   g = val; b = p;   break;
    case 2: r = p;   g = val; b = t;   break;
    case 3: r = p;   g = q;   b = val; break;
    case 4: r = t;   g = p;   b = val; break;
    default: r = val; g = p;  b = q;   break;
  }
}

void ledRainbow() {
  uint16_t hue = (millis() / 20) % 360;  // Full cycle every ~7.2s
  uint8_t r, g, b;
  hsvToRgb(hue, 255, 255, r, g, b);
  setRGB(r, g, b);
}

// ── Turn LED off ────────────────────────────────────────────────────────────

void ledOff() {
  setRGB(0, 0, 0);
}
