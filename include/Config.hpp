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

// ── Device Type ──────────────────────────────────────
// Change this to match your hardware:
// 0=unknown 1=analog  2=digital  3=relay  4=ir  5=hybrid
#define DEVICE_TYPE     1

// ── NVS ─────────────────────────────────────────────
#define NVS_NAMESPACE   "smarthome"
#define NVS_KEY_PROV    "provisioned"
#define NVS_KEY_APIKEY  "api_key"
#define NVS_KEY_GATEWAY "gateway_id"

// ── Factory Reset ───────────────────────────────────
#define RESET_PIN       25
#define RESET_HOLD_MS   5000

#endif // CONFIG_HPP
