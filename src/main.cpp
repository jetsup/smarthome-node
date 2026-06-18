#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include "Config.hpp"

struct __attribute__((__packed__)) ESPNowMessage {
  uint8_t header;
  uint8_t msgType;
  uint32_t deviceId;
  uint16_t value;
  uint8_t checksum;
};

struct __attribute__((__packed__)) ESPNowProvisionMessage {
  uint8_t header;
  uint8_t msgType;
  uint32_t deviceId;
  char apiKey[33];
  char gatewayId[17];
  uint8_t checksum;
};

uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint32_t uniqueNodeId = 0;
Preferences prefs;

bool provisioned = false;
char nodeApiKey[33] = {0};
char nodeGatewayId[17] = {0};

unsigned long lastReport = 0;
unsigned long lastDiscovery = 0;
unsigned long lastMeshFwd = 0;

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
  if (len < 2 || incomingData[0] != 0xAA) {
    Serial.printf("ESPNOW RX: bad header 0x%02X len=%d\n", incomingData[0], len);
    return;
  }

  uint8_t msgType = incomingData[1];
  uint32_t devId = 0;

  if (len >= 6) {
    devId = incomingData[2] | ((uint32_t)incomingData[3] << 8) |
            ((uint32_t)incomingData[4] << 16) | ((uint32_t)incomingData[5] << 24);
  }

  uint8_t calc = 0;
  for (int i = 0; i < len - 1; i++) calc ^= incomingData[i];
  if (calc != incomingData[len - 1]) {
    Serial.printf("ESPNOW RX: checksum mismatch type=%d dev=%u len=%d\n", msgType, devId, len);
    return;
  }

  Serial.printf("ESPNOW RX: type=%d dev=%u len=%d (myId=%u provisioned=%d)\n",
    msgType, devId, len, uniqueNodeId, provisioned);

  switch (msgType) {
    case MSG_COMMAND: {
      if (devId != 0 && devId != uniqueNodeId) break;
      uint16_t val = 0;
      if (len >= 8) {
        val = incomingData[6] | ((uint16_t)incomingData[7] << 8);
      }
      Serial.printf("CMD: value=%u (target=%u me=%u)\n", val, devId, uniqueNodeId);
      if (val == 99) {
        Serial.println("CMD: Factory reset via remote command (value=99)");
        prefs.remove(NVS_KEY_PROV);
        prefs.remove(NVS_KEY_APIKEY);
        prefs.remove(NVS_KEY_GATEWAY);
        prefs.end();
        delay(500);
        ESP.restart();
      } else {
        digitalWrite(LED_BUILTIN, val ? HIGH : LOW);
      }
      break;
    }

    case MSG_SCAN_REQ: {
      Serial.printf("SCAN_REQ: from device %u (provisioned=%d)\n", devId, provisioned);
      if (!provisioned) {
        ESPNowMessage resp;
        resp.header = 0xAA;
        resp.msgType = MSG_DISCOVERY;
        resp.deviceId = uniqueNodeId;
        resp.value = DEVICE_TYPE; // carry device type in value field
        resp.checksum = calcChecksum((uint8_t*)&resp, sizeof(resp));
        sendESPNow((uint8_t*)&resp, sizeof(resp));
        Serial.printf("SCAN_REQ: sent discovery response (myId=%u type=%d)\n", uniqueNodeId, DEVICE_TYPE);
      }
      if (devId != uniqueNodeId && !isDuplicate(devId)) {
        Serial.printf("SCAN_REQ: mesh-forwarding\n");
        sendESPNow(incomingData, len);
      }
      break;
    }

    case MSG_PROVISION: {
      Serial.printf("PROVISION: target=%u me=%u len=%d\n", devId, uniqueNodeId, len);
      if (devId != uniqueNodeId) {
        Serial.printf("PROVISION: not for me (target=%u)\n", devId);
        break;
      }

      if (len >= (int)sizeof(ESPNowProvisionMessage)) {
        ESPNowProvisionMessage provMsg;
        memcpy(&provMsg, incomingData, sizeof(ESPNowProvisionMessage));

        Serial.printf("PROVISION: full message, key starts: %.10s, gateway: %.16s\n",
          provMsg.apiKey, provMsg.gatewayId);

        prefs.putBool(NVS_KEY_PROV, true);
        prefs.putString(NVS_KEY_APIKEY, String(provMsg.apiKey));
        prefs.putString(NVS_KEY_GATEWAY, String(provMsg.gatewayId));
        prefs.end();

        provisioned = true;
        strncpy(nodeApiKey, provMsg.apiKey, sizeof(nodeApiKey) - 1);
        strncpy(nodeGatewayId, provMsg.gatewayId, sizeof(nodeGatewayId) - 1);

        Serial.printf("PROVISION: saved — API key: %s, gateway: %s\n", nodeApiKey, nodeGatewayId);
        Serial.println("PROVISION: rebooting in 500ms");
        delay(500);
        ESP.restart();
      } else if (len >= (int)sizeof(ESPNowMessage)) {
        Serial.println("PROVISION: minimal signal (no full data)");
        prefs.putBool(NVS_KEY_PROV, true);
        prefs.putString(NVS_KEY_APIKEY, "node_provisioned");
        prefs.putString(NVS_KEY_GATEWAY, "unknown");
        prefs.end();

        provisioned = true;
        strcpy(nodeApiKey, "node_provisioned");
        strcpy(nodeGatewayId, "unknown");

        Serial.println("PROVISION: provisioned with defaults, rebooting");
        delay(500);
        ESP.restart();
      } else {
        Serial.printf("PROVISION: packet too short (%d bytes)\n", len);
      }
      break;
    }

    case MSG_DISCOVERY: {
      if (provisioned && devId != uniqueNodeId && !isDuplicate(devId)) {
        Serial.printf("DISCOVERY: mesh-forwarding device %u\n", devId);
        sendESPNow(incomingData, len);
      }
      break;
    }

    default:
      Serial.printf("Unknown msgType=%d from device %u\n", msgType, devId);
      break;
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
      prefs.remove(NVS_KEY_GATEWAY);
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
  Serial.printf("Node boot. ID: %u Type: %d\n", uniqueNodeId, DEVICE_TYPE);

  prefs.begin(NVS_NAMESPACE, false);
  provisioned = prefs.getBool(NVS_KEY_PROV, false);
  String key = prefs.getString(NVS_KEY_APIKEY, "");
  if (key.length() > 0) {
    strncpy(nodeApiKey, key.c_str(), sizeof(nodeApiKey) - 1);
  }
  String gid = prefs.getString(NVS_KEY_GATEWAY, "");
  if (gid.length() > 0) {
    strncpy(nodeGatewayId, gid.c_str(), sizeof(nodeGatewayId) - 1);
  }

  if (provisioned) {
    Serial.printf("State: PROVISIONED (gateway: %s)\n", nodeGatewayId);
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

      // Read sensor based on device type
      switch (DEVICE_TYPE) {
        case 1: // analog
          msg.value = analogRead(34);
          break;
        case 2: // digital
          msg.value = digitalRead(34) ? 1 : 0;
          break;
        case 3: // relay — report current state
          msg.value = 0; // relay state tracked externally
          break;
        case 4: // IR
          msg.value = 0; // IR data handled separately
          break;
        default:
          msg.value = analogRead(34);
          break;
      }

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
      msg.value = DEVICE_TYPE; // carry device type in value field
      msg.checksum = calcChecksum((uint8_t*)&msg, sizeof(msg));

      sendESPNow((uint8_t*)&msg, sizeof(msg));
      Serial.printf("Discovery broadcast: ID %u Type %d\n", uniqueNodeId, DEVICE_TYPE);
    }
  }
}
