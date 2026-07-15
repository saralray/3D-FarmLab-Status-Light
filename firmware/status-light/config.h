#ifndef CONFIG_H
#define CONFIG_H

// ─── Firmware Info ───────────────────────────────────────────────────────────
#define FW_VERSION          "1.0.0"
#define FW_NAME             "3D-FarmLab-Status-Light"

// ─── LED Configuration ──────────────────────────────────────────────────────
// Discrete common-cathode RGB LED (3 color legs + shared GND), driven via
// PWM on 3 GPIOs — NOT an addressable WS2812B. GPIO2/8/9 are ESP32-C3
// strapping pins (boot mode) and are avoided here.
#define LED_PIN_R           3          // Red leg, via current-limiting resistor
#define LED_PIN_G           4          // Green leg, via current-limiting resistor
#define LED_PIN_B           5          // Blue leg, via current-limiting resistor
#define LED_PWM_FREQ        5000       // Hz, above flicker threshold
#define LED_PWM_RESOLUTION  8          // bits — duty cycle range 0-255
#define LED_BRIGHTNESS      255        // 0–255, full brightness

// ─── Network Defaults ───────────────────────────────────────────────────────
#define DEFAULT_POLL_INTERVAL_MS  10000   // 10 seconds
#define WIFI_CONNECT_TIMEOUT_MS   30000  // 30 seconds before giving up
#define HTTP_TIMEOUT_MS           10000  // 10 second HTTP timeout
#define MAX_CONSECUTIVE_ERRORS    3      // Errors before entering error LED state
#define WIFI_RETRY_INTERVAL_MS    30000  // Retry WiFi every 30s on failure

// ─── Serial ─────────────────────────────────────────────────────────────────
#define SERIAL_BAUD         115200

// ─── NVS (Non-Volatile Storage) ─────────────────────────────────────────────
#define NVS_NAMESPACE       "statuslight"
#define NVS_KEY_SSID        "ssid"
#define NVS_KEY_PASSWORD    "password"
#define NVS_KEY_SERVER_URL  "serverUrl"
#define NVS_KEY_PRINTER_ID  "printerId"
#define NVS_KEY_POLL_INT    "pollInt"

// ─── Device States ──────────────────────────────────────────────────────────
enum DeviceState {
  STATE_PROVISIONING,      // No config — waiting for serial provisioning
  STATE_WIFI_CONNECTING,   // Attempting WiFi connection
  STATE_WIFI_FAILED,       // WiFi connection failed
  STATE_RUNNING,           // Normal operation — polling server
  STATE_HTTP_ERROR,        // Consecutive HTTP errors
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

#define COLOR_HTTP_ERR_R    255
#define COLOR_HTTP_ERR_G    0
#define COLOR_HTTP_ERR_B    0

#endif // CONFIG_H
