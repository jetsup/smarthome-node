#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

#define LED_BUILTIN 2

// Packet layout matching the Go Hub and Gateway (9 bytes total)
struct __attribute__((__packed__)) ESPNowMessage {
  uint8_t header;   // 0xAA
  uint8_t msgType;  // 1
  uint32_t deviceId;
  uint16_t value;
  uint8_t checksum;  // XOR of all previous bytes
};  // Total size is 9 bytes

uint8_t gatewayMac[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF};  // Broadcasting to reach the closest mesh link
uint32_t uniqueNodeId = 0;
unsigned long lastExecutionTime = 0;

// Callback triggered when the Gateway sends a command DOWN to the nodes
void onDataRecv(const uint8_t* mac_addr, const uint8_t* incomingData, int len) {
  if (len == sizeof(ESPNowMessage)) {
    ESPNowMessage command;
    memcpy(&command, incomingData, sizeof(ESPNowMessage));

    // CRITICAL CHECK: Only execute if the command is meant for everyone (0) OR
    // specifically for this node's ID
    if (command.deviceId == 0 || command.deviceId == uniqueNodeId) {
      // Example Action: Control onboard LED based on value (1 = ON, 0 = OFF)
      if (command.value == 1) {
        digitalWrite(LED_BUILTIN, HIGH);
      } else if (command.value == 0) {
        digitalWrite(LED_BUILTIN, LOW);
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);

  // Generate a unique 32-bit ID from the ESP32's hardware MAC address
  uint64_t mac = ESP.getEfuseMac();
  uniqueNodeId = (uint32_t)(mac & 0xFFFFFFFF);
  Serial.printf("Node Booted! Unique ID: %u\n", uniqueNodeId);

  // Initialize Radio
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // Register receive handler
  esp_now_register_recv_cb(esp_now_recv_cb_t(onDataRecv));

  // Register broadcast peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, gatewayMac, 6);
  peerInfo.channel = 1;  // Must match Gateway channel
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }
}

void loop() {
  // Non-blocking timer: Send an update or telemetry status upwards every 100ms
  if (millis() - lastExecutionTime > 100) {
    lastExecutionTime = millis();

    ESPNowMessage statusMsg;
    statusMsg.header = 0xAA;
    statusMsg.msgType = 1;
    statusMsg.deviceId = uniqueNodeId;  // Pass our unique identity
    statusMsg.value = analogRead(34);   // Send a random sensor reading or state

    // Calculate XOR Checksum over the first 8 bytes
    uint8_t* ptr = (uint8_t*)&statusMsg;
    uint8_t calcXor = 0;
    for (int i = 0; i < 8; i++) {
      calcXor ^= ptr[i];
    }
    statusMsg.checksum = calcXor;

    // Push packet into the airwaves
    esp_now_send(gatewayMac, (uint8_t*)&statusMsg, sizeof(ESPNowMessage));
    Serial.printf("Status broadcasted from ID: %u Data: %u\n", uniqueNodeId,
                  statusMsg.value);
  }
}
