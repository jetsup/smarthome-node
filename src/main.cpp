#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include "Config.hpp"
#include "capabilities.hpp"

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
  char nodeName[17];
  uint8_t capCount;
  CapabilitySlot caps[CAP_MAX_COUNT];
  uint8_t checksum;
};

uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint32_t uniqueNodeId = 0;
Preferences prefs;

bool provisioned = false;
char nodeApiKey[33] = {0};
char nodeGatewayId[17] = {0};
char nodeName[17] = {0};

uint8_t capCount = 0;
CapabilitySlot caps[CAP_MAX_COUNT];
uint16_t pinValues[CAP_MAX_COUNT] = {0};

unsigned long lastReport = 0;
unsigned long lastDiscovery = 0;
unsigned long lastMeshFwd = 0;
unsigned long lastLedToggle = 0;

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

void handleCommand(uint16_t val) {
  // Apply command value to all capabilities
  for (int i = 0; i < capCount; i++) {
    if (caps[i].type == CAP_ANALOG_OUT) {
      pinValues[i] = val;
      ledcWrite(i, val);
    } else if (caps[i].type == CAP_DIGITAL_OUT || caps[i].type == CAP_RELAY) {
      uint8_t onOff = val ? HIGH : LOW;
      digitalWrite(caps[i].pin, onOff);
      pinValues[i] = onOff;
    }
  }
  digitalWrite(LED_BUILTIN, val ? HIGH : LOW);
}

void handlePinCommand(uint8_t pin, uint8_t val) {
  for (int i = 0; i < capCount; i++) {
    if (caps[i].pin != pin) continue;
    switch (caps[i].type) {
      case CAP_ANALOG_OUT:
        pinValues[i] = val;
        ledcWrite(i, val);
        return;
      case CAP_DIGITAL_OUT:
      case CAP_RELAY:
        digitalWrite(caps[i].pin, val ? HIGH : LOW);
        pinValues[i] = val ? 1 : 0;
        return;
      default:
        return;
    }
  }
}

uint16_t readCapabilityInputs() {
  uint16_t val = 0;
  for (int i = 0; i < capCount; i++) {
    switch (caps[i].type) {
      case CAP_ANALOG_IN:
        val = analogRead(caps[i].pin);
        pinValues[i] = val;
        break;
      case CAP_DIGITAL_IN:
        val = digitalRead(caps[i].pin) ? 1 : 0;
        pinValues[i] = val;
        break;
      case CAP_ANALOG_OUT:
      case CAP_DIGITAL_OUT:
      case CAP_RELAY:
        val = pinValues[i];
        break;
      default:
        break;
    }
  }
  return val;
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
        prefs.remove(NVS_KEY_NAME);
        prefs.remove(NVS_KEY_CAPS);
        prefs.end();
        delay(500);
        ESP.restart();
      } else {
        handleCommand(val);
      }
      break;
    }

    case MSG_PIN_CMD: {
      if (devId != 0 && devId != uniqueNodeId) break;
      if (len < 8) break;
      uint8_t pin = incomingData[6];
      uint8_t val = incomingData[7];
      Serial.printf("PIN_CMD: pin=%u val=%u\n", pin, val);
      handlePinCommand(pin, val);
      break;
    }

    case MSG_SCAN_REQ: {
      Serial.printf("SCAN_REQ: from device %u (provisioned=%d)\n", devId, provisioned);
      if (!provisioned) {
        ESPNowMessage resp;
        resp.header = 0xAA;
        resp.msgType = MSG_DISCOVERY;
        resp.deviceId = uniqueNodeId;
        resp.value = DEVICE_TYPE;
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

        Serial.printf("PROVISION: full message, key starts: %.10s, gateway: %.16s, name=%s\n",
          provMsg.apiKey, provMsg.gatewayId, provMsg.nodeName);

        prefs.putBool(NVS_KEY_PROV, true);
        prefs.putString(NVS_KEY_APIKEY, String(provMsg.apiKey));
        prefs.putString(NVS_KEY_GATEWAY, String(provMsg.gatewayId));
        prefs.putString(NVS_KEY_NAME, String(provMsg.nodeName));
        prefs.putString(NVS_KEY_CAPS, serializeCaps(provMsg.caps, provMsg.capCount));
        prefs.end();

        provisioned = true;
        strncpy(nodeApiKey, provMsg.apiKey, sizeof(nodeApiKey) - 1);
        strncpy(nodeGatewayId, provMsg.gatewayId, sizeof(nodeGatewayId) - 1);
        strncpy(nodeName, provMsg.nodeName, sizeof(nodeName) - 1);
        capCount = provMsg.capCount > CAP_MAX_COUNT ? CAP_MAX_COUNT : provMsg.capCount;
        memcpy(caps, provMsg.caps, capCount * sizeof(CapabilitySlot));

        applyCapPinModes(caps, capCount);

        Serial.printf("PROVISION: saved — API key: %s, gateway: %s, name: %s, caps: %d\n",
          nodeApiKey, nodeGatewayId, nodeName, capCount);
        Serial.println("PROVISION: rebooting in 500ms");
        delay(500);
        ESP.restart();
      } else if (len >= (int)sizeof(ESPNowMessage)) {
        Serial.println("PROVISION: minimal signal (no full data)");
        prefs.putBool(NVS_KEY_PROV, true);
        prefs.putString(NVS_KEY_APIKEY, "node_provisioned");
        prefs.putString(NVS_KEY_GATEWAY, "unknown");
        prefs.putString(NVS_KEY_NAME, "");
        prefs.putString(NVS_KEY_CAPS, "");
        prefs.end();

        provisioned = true;
        strcpy(nodeApiKey, "node_provisioned");
        strcpy(nodeGatewayId, "unknown");
        nodeName[0] = '\0';
        capCount = 0;

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
      prefs.remove(NVS_KEY_NAME);
      prefs.remove(NVS_KEY_CAPS);
      prefs.end();
      delay(500);
      ESP.restart();
    }
  } else {
    pressStart = 0;
  }
}

void parseAndApplyCaps(const String& capsStr) {
  capCount = deserializeCaps(caps, capsStr);
  applyCapPinModes(caps, capCount);
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
  String name = prefs.getString(NVS_KEY_NAME, "");
  if (name.length() > 0) {
    strncpy(nodeName, name.c_str(), sizeof(nodeName) - 1);
  }
  String capsStr = prefs.getString(NVS_KEY_CAPS, "");
  if (capsStr.length() > 0) {
    parseAndApplyCaps(capsStr);
  }

  // Read analog input pins on boot so reported values reflect actual pin state
  for (int i = 0; i < capCount; i++) {
    if (caps[i].type == CAP_ANALOG_IN) {
      pinValues[i] = analogRead(caps[i].pin);
    } else if (caps[i].type == CAP_DIGITAL_IN) {
      pinValues[i] = digitalRead(caps[i].pin) ? 1 : 0;
    }
  }

  if (provisioned) {
    Serial.printf("State: PROVISIONED (gateway: %s, name: %s, caps: %d)\n",
      nodeGatewayId, nodeName, capCount);
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
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_TIMEOUT_MS) {
    delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected on channel %d\n", WiFi.channel());
  } else {
    Serial.println("WiFi not available — ESP-NOW on default channel");
  }

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

      msg.value = readCapabilityInputs();

      msg.checksum = calcChecksum((uint8_t*)&msg, sizeof(msg));

      sendESPNow((uint8_t*)&msg, sizeof(msg));
    }
  } else {
    // Blink LED every 100ms during discovery mode
    if (now - lastLedToggle >= 100) {
      lastLedToggle = now;
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }

    if (now - lastDiscovery > DISCOVERY_INTERVAL_MS) {
      lastDiscovery = now;

      ESPNowMessage msg;
      msg.header = 0xAA;
      msg.msgType = MSG_DISCOVERY;
      msg.deviceId = uniqueNodeId;
      msg.value = DEVICE_TYPE;
      msg.checksum = calcChecksum((uint8_t*)&msg, sizeof(msg));

      sendESPNow((uint8_t*)&msg, sizeof(msg));
      Serial.printf("Discovery broadcast: ID %u Type %d\n", uniqueNodeId, DEVICE_TYPE);
    }
  }
}
