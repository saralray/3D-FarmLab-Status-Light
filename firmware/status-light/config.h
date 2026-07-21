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
#define WIFI_CONNECT_TIMEOUT_MS   30000  // 30 seconds before giving up
#define WIFI_RETRY_INTERVAL_MS    30000  // Retry WiFi every 30s on failure

// ─── Serial ─────────────────────────────────────────────────────────────────
#define SERIAL_BAUD         115200

// ─── NVS (Non-Volatile Storage) ─────────────────────────────────────────────
// The status light gets its printer status pushed over MQTT from the dashboard's
// embedded broker (server/statusLightBroker.js), not by polling HTTP. It
// subscribes to printfarm/printers/<printerId>/status and publishes liveness to
// printfarm/lights/<printerId>/availability (also the MQTT LWT).
#define NVS_NAMESPACE       "statuslight"
#define NVS_KEY_SSID        "ssid"
#define NVS_KEY_PASSWORD    "password"
#define NVS_KEY_MQTT_TRANS  "mqttTrans"   // "tcp" | "ws" | "wss"
#define NVS_KEY_MQTT_HOST   "mqttHost"
#define NVS_KEY_MQTT_PORT   "mqttPort"
#define NVS_KEY_MQTT_PATH   "mqttPath"    // ws/wss only, e.g. "/mqtt"
#define NVS_KEY_MQTT_USER   "mqttUser"
#define NVS_KEY_MQTT_PASS   "mqttPass"
#define NVS_KEY_PRINTER_ID  "printerId"

// ─── Device States ──────────────────────────────────────────────────────────
enum DeviceState {
  STATE_PROVISIONING,      // No config — waiting for serial provisioning
  STATE_WIFI_CONNECTING,   // Attempting WiFi connection
  STATE_WIFI_FAILED,       // WiFi connection failed
  STATE_MQTT_CONNECTING,   // WiFi up, connecting/subscribing to the broker
  STATE_RUNNING,           // Connected & subscribed — showing pushed status
  STATE_MQTT_ERROR,        // Lost the broker connection
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

#define COLOR_MQTT_ERR_R    255
#define COLOR_MQTT_ERR_G    0
#define COLOR_MQTT_ERR_B    0

#endif // CONFIG_H
