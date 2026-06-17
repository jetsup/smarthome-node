#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include "Config.hpp"

// Packet layout matching the Go Hub and Gateway (9 bytes total)
struct __attribute__((__packed__)) ESPNowMessage {
  uint8_t header;   // 0xAA
  uint8_t msgType;  // 1
  uint32_t deviceId;
  uint16_t value;
  uint8_t checksum;  // XOR of all previous bytes
};  // Total size is 9 bytes

struct __attribute__((__packed__)) ESPNowProvisionMessage {
  uint8_t header;
  uint8_t msgType;
  uint32_t deviceId;
  char apiKey[33];
  uint8_t checksum;
};

uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint32_t uniqueNodeId = 0;
Preferences prefs;

bool provisioned = false;
char nodeApiKey[33] = {0};

unsigned long lastReport = 0;
unsigned long lastDiscovery = 0;
unsigned long lastMeshFwd = 0;

// Simple dedup for mesh forwarding: track recently seen discovery deviceIds
#define DEDUP_SIZE 16
uint32_t dedupIds[DEDUP_SIZE];
unsigned long dedupTimes[DEDUP_SIZE];
int dedupIndex = 0;

bool isDuplicate(uint32_t devId) {
  for (int i = 0; i < DEDUP_SIZE; i++) {
    if (dedupIds[i] == devId && (millis() - dedupTimes[i] < MESH_FORWARD_INTERVAL_MS)) {
      return true;
    }
  }
  dedupIds[dedupIndex] = devId;
  dedupTimes[dedupIndex] = millis();
  dedupIndex = (dedupIndex + 1) % DEDUP_SIZE;
  return false;
}

uint8_t calcChecksum(const uint8_t* data, int len) {
  uint8_t xorVal = 0;
  for (int i = 0; i < len - 1; i++) {
    xorVal ^= data[i];
  }
  return xorVal;
}

void sendESPNow(const uint8_t* data, int len) {
  esp_now_send(broadcastMac, data, len);
}

void onDataRecv(const uint8_t* mac_addr, const uint8_t* incomingData, int len) {
  if (len < 2 || incomingData[0] != 0xAA) return;

  uint8_t msgType = incomingData[1];
  uint32_t devId = 0;

  if (len >= 6) {
    devId = incomingData[2] | ((uint32_t)incomingData[3] << 8) |
            ((uint32_t)incomingData[4] << 16) | ((uint32_t)incomingData[5] << 24);
  }

  // Validate checksum
  uint8_t calc = 0;
  for (int i = 0; i < len - 1; i++) calc ^= incomingData[i];
  if (calc != incomingData[len - 1]) return;

  switch (msgType) {
    case MSG_COMMAND: {
      if (devId != 0 && devId != uniqueNodeId) break;
      uint16_t val = 0;
      if (len >= 8) {
        val = incomingData[6] | ((uint16_t)incomingData[7] << 8);
      }
      if (val == 99) {
        Serial.println("Factory reset via remote command");
        prefs.remove(NVS_KEY_PROV);
        prefs.remove(NVS_KEY_APIKEY);
        prefs.end();
        delay(500);
        ESP.restart();
      } else {
        digitalWrite(LED_BUILTIN, val ? HIGH : LOW);
      }
      Serial.printf("Command: value=%u\n", val);
      break;
    }

    case MSG_SCAN_REQ: {
      // Respond with discovery if unprovisioned
      if (!provisioned) {
        ESPNowMessage resp;
        resp.header = 0xAA;
        resp.msgType = MSG_DISCOVERY;
        resp.deviceId = uniqueNodeId;
        resp.value = 0;
        resp.checksum = calcChecksum((uint8_t*)&resp, sizeof(resp));
        sendESPNow((uint8_t*)&resp, sizeof(resp));
        Serial.println("Responded to scan request");
      }
      // Mesh forward scan request (both provisioned and unprovisioned nodes relay)
      if (devId != uniqueNodeId && !isDuplicate(devId)) {
        sendESPNow(incomingData, len);
      }
      break;
    }

    case MSG_PROVISION: {
      // Provision command — only for us
      if (devId != uniqueNodeId) break;

      if (len >= (int)sizeof(ESPNowProvisionMessage)) {
        ESPNowProvisionMessage provMsg;
        memcpy(&provMsg, incomingData, sizeof(ESPNowProvisionMessage));

        prefs.putBool(NVS_KEY_PROV, true);
        prefs.putString(NVS_KEY_APIKEY, String(provMsg.apiKey));
        prefs.end();

        provisioned = true;
        strncpy(nodeApiKey, provMsg.apiKey, sizeof(nodeApiKey) - 1);

        Serial.printf("Provisioned! API key: %s\n", nodeApiKey);
        delay(500);
        ESP.restart();
      } else if (len >= (int)sizeof(ESPNowMessage)) {
        // Fallback: minimal provision signal (no API key in payload)
        // Just mark as provisioned with a default key
        prefs.putBool(NVS_KEY_PROV, true);
        prefs.putString(NVS_KEY_APIKEY, "node_provisioned");
        prefs.end();

        provisioned = true;
        strcpy(nodeApiKey, "node_provisioned");

        Serial.println("Provisioned (minimal signal)");
        delay(500);
        ESP.restart();
      }
      break;
    }

    case MSG_DISCOVERY: {
      // Mesh forwarding: if provisioned, re-broadcast discovery from other nodes
      if (provisioned && devId != uniqueNodeId && !isDuplicate(devId)) {
        sendESPNow(incomingData, len);
      }
      break;
    }
  }
}

void checkResetPin() {
  static unsigned long pressStart = 0;
  if (digitalRead(RESET_PIN) == LOW) {
    if (pressStart == 0) {
      pressStart = millis();
    } else if (millis() - pressStart >= RESET_HOLD_MS) {
      Serial.println("Factory reset via GPIO " + String(RESET_PIN));
      prefs.remove(NVS_KEY_PROV);
      prefs.remove(NVS_KEY_APIKEY);
      prefs.end();
      delay(500);
      ESP.restart();
    }
  } else {
    pressStart = 0;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(RESET_PIN, INPUT_PULLUP);

  uint64_t mac = ESP.getEfuseMac();
  uniqueNodeId = (uint32_t)(mac & 0xFFFFFFFF);
  Serial.printf("Node boot. ID: %u\n", uniqueNodeId);

  // Load provisioning state from NVS
  prefs.begin(NVS_NAMESPACE, false);
  provisioned = prefs.getBool(NVS_KEY_PROV, false);
  String key = prefs.getString(NVS_KEY_APIKEY, "");
  if (key.length() > 0) {
    strncpy(nodeApiKey, key.c_str(), sizeof(nodeApiKey) - 1);
  }

  if (provisioned) {
    Serial.println("State: PROVISIONED");
  } else {
    Serial.println("State: UNPROVISIONED — broadcasting discovery");
    digitalWrite(LED_BUILTIN, HIGH);
    delay(200);
    digitalWrite(LED_BUILTIN, LOW);
    delay(200);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(200);
    digitalWrite(LED_BUILTIN, LOW);
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(esp_now_recv_cb_t(onDataRecv));

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastMac, 6);
  peerInfo.channel = ESPNOW_CHANNEL;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }
}

void loop() {
  checkResetPin();
  unsigned long now = millis();

  if (provisioned) {
    if (now - lastReport > REPORT_INTERVAL_MS) {
      lastReport = now;

      ESPNowMessage msg;
      msg.header = 0xAA;
      msg.msgType = MSG_TELEMETRY;
      msg.deviceId = uniqueNodeId;
      msg.value = analogRead(34);
      msg.checksum = calcChecksum((uint8_t*)&msg, sizeof(msg));

      sendESPNow((uint8_t*)&msg, sizeof(msg));
    }
  } else {
    if (now - lastDiscovery > DISCOVERY_INTERVAL_MS) {
      lastDiscovery = now;

      ESPNowMessage msg;
      msg.header = 0xAA;
      msg.msgType = MSG_DISCOVERY;
      msg.deviceId = uniqueNodeId;
      msg.value = 0;
      msg.checksum = calcChecksum((uint8_t*)&msg, sizeof(msg));

      sendESPNow((uint8_t*)&msg, sizeof(msg));
      Serial.printf("Discovery broadcast: ID %u\n", uniqueNodeId);
    }
  }
}
