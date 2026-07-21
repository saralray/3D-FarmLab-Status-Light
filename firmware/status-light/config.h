#ifndef CONFIG_H
#define CONFIG_H

// ─── Firmware Info ───────────────────────────────────────────────────────────
#define FW_VERSION          "1.0.0"
#define FW_NAME             "3D-FarmLab-Status-Light"

// ─── LED Configuration ──────────────────────────────────────────────────────
// Discrete common-cathode RGB LED (3 color legs + shared GND), driven with
// plain digital on/off on 3 GPIOs — NOT an addressable WS2812B, NOT PWM.
// GPIO2/8/9 are ESP32-C3 strapping pins (boot mode) and are avoided here.
#define LED_PIN_R           3          // Red leg, via current-limiting resistor
#define LED_PIN_G           4          // Green leg, via current-limiting resistor
#define LED_PIN_B           5          // Blue leg, via current-limiting resistor

// ─── Network Defaults ───────────────────────────────────────────────────────
#define DEFAULT_MQTT_PORT         1883   // Broker raw-TCP port (host MQTT_PORT)
#define MQTT_KEEPALIVE_S          15     // MQTT keepalive (seconds)
#define DEFAULT_POLL_INTERVAL_MS  10000  // API mode: status poll cadence
#define HTTP_TIMEOUT_MS           10000  // API mode: per-request HTTP timeout
#define MAX_CONSECUTIVE_ERRORS    3      // API mode: fails before LINK_ERROR
#define WIFI_CONNECT_TIMEOUT_MS   30000  // 30 seconds before giving up
#define WIFI_RETRY_INTERVAL_MS    30000  // Retry WiFi every 30s on failure

// ─── Serial ─────────────────────────────────────────────────────────────────
#define SERIAL_BAUD         115200

// ─── NVS (Non-Volatile Storage) ─────────────────────────────────────────────
// The status light can get its printer status two ways, chosen at provisioning
// via the "mode" field:
//   - "mqtt" (default): subscribe to the dashboard's embedded MQTT broker
//     (server/statusLightBroker.js) topic printfarm/printers/<printerId>/status
//     and publish liveness to printfarm/lights/<printerId>/availability (LWT).
//   - "api": poll GET <serverUrl>/api/status-light/printers/<printerId> every
//     pollInterval ms over plain HTTP(S).
#define NVS_NAMESPACE       "statuslight"
#define NVS_KEY_SSID        "ssid"
#define NVS_KEY_PASSWORD    "password"
#define NVS_KEY_MODE        "mode"        // "mqtt" | "api"
#define NVS_KEY_PRINTER_ID  "printerId"
// MQTT mode
#define NVS_KEY_MQTT_TRANS  "mqttTrans"   // "tcp" | "ws" | "wss"
#define NVS_KEY_MQTT_HOST   "mqttHost"
#define NVS_KEY_MQTT_PORT   "mqttPort"
#define NVS_KEY_MQTT_PATH   "mqttPath"    // ws/wss only, e.g. "/mqtt"
#define NVS_KEY_MQTT_USER   "mqttUser"
#define NVS_KEY_MQTT_PASS   "mqttPass"
// API mode
#define NVS_KEY_SERVER_URL  "serverUrl"
#define NVS_KEY_POLL_INT    "pollInt"

// ─── Device States ──────────────────────────────────────────────────────────
enum DeviceState {
  STATE_PROVISIONING,      // No config — waiting for serial provisioning
  STATE_WIFI_CONNECTING,   // Attempting WiFi connection
  STATE_WIFI_FAILED,       // WiFi connection failed
  STATE_MQTT_CONNECTING,   // MQTT mode: WiFi up, connecting/subscribing to broker
  STATE_RUNNING,           // Connected — showing status (subscribed or polling)
  STATE_LINK_ERROR,        // Lost the status link (broker dropped / poll failing)
};

// ─── Printer Status ─────────────────────────────────────────────────────────
enum PrinterStatus {
  STATUS_UNKNOWN,
  STATUS_IDLE,
  STATUS_PRINTING,
  STATUS_PAUSED,
  STATUS_ERROR,
  STATUS_OFFLINE,
};

// ─── LED Colors (R, G, B) ───────────────────────────────────────────────────
#define COLOR_IDLE_R        0
#define COLOR_IDLE_G        180
#define COLOR_IDLE_B        0

#define COLOR_PRINTING_R    0
#define COLOR_PRINTING_G    80
#define COLOR_PRINTING_B    255

#define COLOR_PAUSED_R      255
#define COLOR_PAUSED_G      160
#define COLOR_PAUSED_B      0

#define COLOR_ERROR_R       255
#define COLOR_ERROR_G       0
#define COLOR_ERROR_B       0

#define COLOR_OFFLINE_R     40
#define COLOR_OFFLINE_G     40
#define COLOR_OFFLINE_B     40

#define COLOR_WIFI_R        0
#define COLOR_WIFI_G        180
#define COLOR_WIFI_B        180

#define COLOR_LINK_ERR_R    255
#define COLOR_LINK_ERR_G    0
#define COLOR_LINK_ERR_B    0

#endif // CONFIG_H
