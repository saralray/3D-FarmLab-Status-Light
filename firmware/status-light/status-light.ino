/*
 * 3D FarmLab — ESP32-C3 Printer Status Light
 * ============================================
 *
 * Firmware that subscribes to a 3D printer farm dashboard's embedded MQTT
 * broker (server/statusLightBroker.js) for printer status and displays it via
 * a discrete common-cathode RGB LED, driven with plain digital on/off on 3
 * GPIOs.
 *
 * Features:
 *   - Reads WiFi + MQTT broker config from NVS (non-volatile storage)
 *   - Serial provisioning protocol (JSON commands at 115200 baud)
 *   - Non-blocking LED animations for each status
 *   - Auto-reconnect on WiFi loss (and MQTT reconnect via esp-mqtt)
 *   - Error state when the broker connection is lost
 *
 * Status is PUSHED, not polled: the device subscribes to
 *   printfarm/printers/<printerId>/status   (retained, plain string)
 * and publishes its own liveness to
 *   printfarm/lights/<printerId>/availability  (retained; also the MQTT LWT)
 *
 * Hardware:
 *   - ESP32-C3 (Super Mini or Dev Module)
 *   - Common-cathode RGB LED: R/G/B legs on GPIO3/4/5, common leg to GND,
 *     driven as plain digital on/off outputs (no PWM)
 *
 * Libraries (install via Arduino Library Manager):
 *   - ArduinoJson (v7+)
 *   (The MQTT client is esp-mqtt, part of the ESP-IDF bundled with the
 *    Arduino-ESP32 core — no extra library needed.)
 *
 * License: MIT
 */

#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <mqtt_client.h>
#if __has_include("esp_crt_bundle.h")
#include "esp_crt_bundle.h"
#define STATUSLIGHT_HAVE_CRT_BUNDLE 1
#endif
#include "config.h"

// ─── Globals ────────────────────────────────────────────────────────────────

Preferences prefs;

// Config from NVS
String cfgSSID          = "";
String cfgPassword      = "";
String cfgMqttTransport = "tcp";   // "tcp" | "ws" | "wss"
String cfgMqttHost      = "";
uint16_t cfgMqttPort    = DEFAULT_MQTT_PORT;
String cfgMqttPath      = "/mqtt"; // ws/wss only
String cfgMqttUser      = "";
String cfgMqttPass      = "";
String cfgPrinterId     = "";

// Runtime state
DeviceState  deviceState       = STATE_PROVISIONING;
PrinterStatus printerStatus    = STATUS_UNKNOWN;
unsigned long lastWifiAttempt  = 0;
unsigned long stateEnteredAt   = 0;

// MQTT
esp_mqtt_client_handle_t mqttClient = nullptr;
String mqttClientId;
String mqttStatusTopic;
String mqttAvailabilityTopic;

// Serial input buffer
String serialBuffer = "";

// ─── Forward Declarations ───────────────────────────────────────────────────

void loadConfig();
void saveConfig();
void clearConfig();
void handleSerial();
void processSerialCommand(const String& line);
void connectWiFi();
void startMqtt();
void stopMqtt();
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
  pinMode(LED_PIN_R, OUTPUT);
  pinMode(LED_PIN_G, OUTPUT);
  pinMode(LED_PIN_B, OUTPUT);

  // Boot self-test: prove the LED hardware + wiring work under firmware.
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

  if (cfgSSID.length() == 0 || cfgMqttHost.length() == 0 || cfgPrinterId.length() == 0) {
    Serial.println("[BOOT] No configuration found. Entering provisioning mode.");
    Serial.println("[BOOT] Send JSON config via serial to provision this device.");
    Serial.println("[BOOT] Example: {\"cmd\":\"provision\",\"ssid\":\"...\",\"password\":\"...\",\"mqttTransport\":\"tcp\",\"mqttHost\":\"10.0.0.5\",\"mqttPort\":1883,\"mqttUsername\":\"statuslight\",\"mqttPassword\":\"...\",\"printerId\":\"...\"}");
    setDeviceState(STATE_PROVISIONING);
  } else {
    Serial.println("[BOOT] Configuration loaded:");
    Serial.println("  SSID:        " + cfgSSID);
    Serial.println("  MQTT:        " + cfgMqttTransport + "://" + cfgMqttHost + ":" + String(cfgMqttPort));
    Serial.println("  Printer ID:  " + cfgPrinterId);
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
        startMqtt();  // moves to STATE_MQTT_CONNECTING
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

    case STATE_MQTT_CONNECTING:
    case STATE_RUNNING:
    case STATE_MQTT_ERROR:
      // If WiFi drops, tear down MQTT and go back to reconnecting WiFi. The
      // MQTT connect/subscribe and status pushes are driven entirely by the
      // esp-mqtt event handler; nothing to poll here.
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] Disconnected! Reconnecting...");
        stopMqtt();
        setDeviceState(STATE_WIFI_CONNECTING);
        connectWiFi();
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
  cfgSSID          = prefs.getString(NVS_KEY_SSID, "");
  cfgPassword      = prefs.getString(NVS_KEY_PASSWORD, "");
  cfgMqttTransport = prefs.getString(NVS_KEY_MQTT_TRANS, "tcp");
  cfgMqttHost      = prefs.getString(NVS_KEY_MQTT_HOST, "");
  cfgMqttPort      = prefs.getUShort(NVS_KEY_MQTT_PORT, DEFAULT_MQTT_PORT);
  cfgMqttPath      = prefs.getString(NVS_KEY_MQTT_PATH, "/mqtt");
  cfgMqttUser      = prefs.getString(NVS_KEY_MQTT_USER, "");
  cfgMqttPass      = prefs.getString(NVS_KEY_MQTT_PASS, "");
  cfgPrinterId     = prefs.getString(NVS_KEY_PRINTER_ID, "");
  prefs.end();
}

void saveConfig() {
  prefs.begin(NVS_NAMESPACE, false);  // Read-write
  prefs.putString(NVS_KEY_SSID, cfgSSID);
  prefs.putString(NVS_KEY_PASSWORD, cfgPassword);
  prefs.putString(NVS_KEY_MQTT_TRANS, cfgMqttTransport);
  prefs.putString(NVS_KEY_MQTT_HOST, cfgMqttHost);
  prefs.putUShort(NVS_KEY_MQTT_PORT, cfgMqttPort);
  prefs.putString(NVS_KEY_MQTT_PATH, cfgMqttPath);
  prefs.putString(NVS_KEY_MQTT_USER, cfgMqttUser);
  prefs.putString(NVS_KEY_MQTT_PASS, cfgMqttPass);
  prefs.putString(NVS_KEY_PRINTER_ID, cfgPrinterId);
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
    const char* transport = doc["mqttTransport"] | "tcp";
    const char* host      = doc["mqttHost"] | "";
    uint16_t    port      = (uint16_t)(doc["mqttPort"] | DEFAULT_MQTT_PORT);
    const char* path      = doc["mqttPath"] | "/mqtt";
    const char* user      = doc["mqttUsername"] | "";
    const char* pass      = doc["mqttPassword"] | "";
    const char* printerId = doc["printerId"] | "";

    if (strlen(ssid) == 0 || strlen(host) == 0 || strlen(printerId) == 0) {
      Serial.println("{\"status\":\"error\",\"msg\":\"Missing required fields: ssid, mqttHost, printerId\"}");
      return;
    }

    cfgSSID          = String(ssid);
    cfgPassword      = String(password);
    cfgMqttTransport = String(transport);
    cfgMqttHost      = String(host);
    cfgMqttPort      = port;
    cfgMqttPath      = String(path);
    cfgMqttUser      = String(user);
    cfgMqttPass      = String(pass);
    cfgPrinterId     = String(printerId);

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
    resp["mqttTransport"] = cfgMqttTransport;
    resp["mqttHost"]      = cfgMqttHost;
    resp["mqttPort"]      = cfgMqttPort;
    resp["printerId"]     = cfgPrinterId;
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

// ─── MQTT ───────────────────────────────────────────────────────────────────

// mqtt://host:port for LAN, ws(s)://host:port/mqtt through nginx — the same
// broker either way (server/statusLightBroker.js).
static String buildBrokerUri() {
  String scheme = cfgMqttTransport;
  if (scheme != "ws" && scheme != "wss") {
    scheme = "mqtt";
  }
  String uri = scheme + "://" + cfgMqttHost + ":" + String(cfgMqttPort);
  if (scheme != "mqtt") {
    uri += cfgMqttPath.startsWith("/") ? cfgMqttPath : "/" + cfgMqttPath;
  }
  return uri;
}

static void mqttEventHandler(void*, esp_event_base_t, int32_t eventId, void* eventData) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)eventData;
  switch ((esp_mqtt_event_id_t)eventId) {
    case MQTT_EVENT_CONNECTED:
      Serial.println("[MQTT] Connected — subscribing to " + mqttStatusTopic);
      esp_mqtt_client_publish(mqttClient, mqttAvailabilityTopic.c_str(), "online", 0, 0, 1);
      // Retained status arrives immediately after this subscribe.
      esp_mqtt_client_subscribe(mqttClient, mqttStatusTopic.c_str(), 0);
      setDeviceState(STATE_RUNNING);
      break;
    case MQTT_EVENT_DISCONNECTED:
      // esp-mqtt auto-reconnects; reflect the drop for the LED until it's back.
      if (deviceState == STATE_RUNNING) {
        setDeviceState(STATE_MQTT_ERROR);
      }
      break;
    case MQTT_EVENT_DATA:
      if (event->topic_len > 0 &&
          mqttStatusTopic.equals(String(event->topic, event->topic_len))) {
        String payload(event->data, event->data_len);
        Serial.println("[MQTT] status: " + payload);
        // Empty retained payload = printer deleted on the server.
        printerStatus = payload.length() == 0 ? STATUS_UNKNOWN : parseStatus(payload);
      }
      break;
    default:
      break;
  }
}

void startMqtt() {
  stopMqtt();
  mqttClientId          = "statuslight-" + cfgPrinterId;
  mqttStatusTopic       = "printfarm/printers/" + cfgPrinterId + "/status";
  mqttAvailabilityTopic = "printfarm/lights/" + cfgPrinterId + "/availability";

  String uri = buildBrokerUri();
  Serial.println("[MQTT] Connecting to " + uri);

  esp_mqtt_client_config_t mqttConfig = {};
  mqttConfig.uri        = uri.c_str();
  mqttConfig.client_id  = mqttClientId.c_str();
  mqttConfig.username   = cfgMqttUser.c_str();
  mqttConfig.password   = cfgMqttPass.c_str();
  mqttConfig.keepalive  = MQTT_KEEPALIVE_S;
  mqttConfig.lwt_topic  = mqttAvailabilityTopic.c_str();
  mqttConfig.lwt_msg    = "offline";
  mqttConfig.lwt_qos    = 0;
  mqttConfig.lwt_retain = 1;
#ifdef STATUSLIGHT_HAVE_CRT_BUNDLE
  if (cfgMqttTransport == "wss") {
    // Validate the wss certificate against the built-in public-CA bundle (works
    // with Let's Encrypt etc.; a self-signed cert needs "ws" instead).
    mqttConfig.crt_bundle_attach = arduino_esp_crt_bundle_attach;
  }
#endif

  mqttClient = esp_mqtt_client_init(&mqttConfig);
  esp_mqtt_client_register_event(mqttClient, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqttEventHandler, nullptr);
  esp_mqtt_client_start(mqttClient);
  setDeviceState(STATE_MQTT_CONNECTING);
}

void stopMqtt() {
  if (mqttClient) {
    esp_mqtt_client_stop(mqttClient);
    esp_mqtt_client_destroy(mqttClient);
    mqttClient = nullptr;
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
    case STATE_MQTT_CONNECTING:
      ledBlink(COLOR_WIFI_R, COLOR_WIFI_G, COLOR_WIFI_B, 4.0);  // 4Hz fast blink
      break;

    case STATE_WIFI_FAILED:
      ledBreathing(COLOR_ERROR_R, COLOR_ERROR_G, COLOR_ERROR_B, 0.5);  // Slow red pulse
      break;

    case STATE_MQTT_ERROR:
      ledTripleBlink(COLOR_MQTT_ERR_R, COLOR_MQTT_ERR_G, COLOR_MQTT_ERR_B);
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
