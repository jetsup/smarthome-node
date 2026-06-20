#ifndef CONFIG_HPP
#define CONFIG_HPP

// ── ESP-NOW ─────────────────────────────────────────
#define ESPNOW_CHANNEL  0

// ── Device ──────────────────────────────────────────
#define LED_BUILTIN     2

// ── Timing ──────────────────────────────────────────
#define REPORT_INTERVAL_MS  100
#define DISCOVERY_INTERVAL_MS 3000
#define MESH_FORWARD_INTERVAL_MS 2000

// ── ESP-NOW Message Types ───────────────────────────
#define MSG_TELEMETRY   1
#define MSG_COMMAND     2
#define MSG_DISCOVERY   3
#define MSG_SCAN_REQ    4
#define MSG_PROVISION   5
#define MSG_PIN_CMD     6

// ── Device Type ──────────────────────────────────────
// Change this to match your hardware:
// 0=unknown 1=analog  2=digital  3=relay  4=ir  5=hybrid
#define DEVICE_TYPE     1

// ── Capability type constants ────────────────────────
#define CAP_ANALOG_IN   0
#define CAP_ANALOG_OUT  1
#define CAP_DIGITAL_IN  2
#define CAP_DIGITAL_OUT 3
#define CAP_RELAY       4
#define CAP_IR_TX       5
#define CAP_IR_RX       6
#define CAP_I2C         7
#define CAP_UART        8

#define CAP_MAX_COUNT   8
#define CAP_LABEL_LEN   11

// ── NVS ─────────────────────────────────────────────
#define NVS_NAMESPACE   "smarthome"
#define NVS_KEY_PROV    "provisioned"
#define NVS_KEY_APIKEY  "api_key"
#define NVS_KEY_GATEWAY "gateway_id"
#define NVS_KEY_NAME    "node_name"
#define NVS_KEY_CAPS    "node_caps"

// ── Factory Reset ───────────────────────────────────
#define RESET_PIN       25
#define RESET_HOLD_MS   5000

// ── WiFi (connect to lock radio channel with gateway) ──
#define WIFI_SSID       "Qt"
#define WIFI_PASS       "11223344"
#define WIFI_TIMEOUT_MS 15000

#endif // CONFIG_HPP
