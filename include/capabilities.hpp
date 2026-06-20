#ifndef CAPABILITIES_HPP
#define CAPABILITIES_HPP

#include <Arduino.h>
#include "Config.hpp"

// ── Capability slot (matching gateway's CapabilitySlot) ─────────────────────
struct __attribute__((__packed__)) CapabilitySlot {
  uint8_t type;
  uint8_t pin;
  uint8_t extra;          // I2C address or UART number
  char label[CAP_LABEL_LEN];
};

// ── Default pin assignments per capability type ─────────────────────────────
// Return a default pin for a given capability type (0 means not applicable).
inline uint8_t defaultPinForCapType(uint8_t type) {
  switch (type) {
    case CAP_ANALOG_OUT:  return 26;
    case CAP_DIGITAL_OUT: return 2;
    case CAP_ANALOG_IN:   return 34;
    case CAP_DIGITAL_IN:  return 35;
    case CAP_RELAY:       return 32;
    case CAP_I2C:         return 0;
    case CAP_UART:        return 0;
    case CAP_IR_TX:       return 26;
    case CAP_IR_RX:       return 27;
    default:              return 0;
  }
}

// ── Type name to constant ───────────────────────────────────────────────────
inline uint8_t capTypeFromName(const String& name) {
  if (name == "analogInput")   return CAP_ANALOG_IN;
  if (name == "analogOutput")  return CAP_ANALOG_OUT;
  if (name == "digitalInput")  return CAP_DIGITAL_IN;
  if (name == "digitalOutput") return CAP_DIGITAL_OUT;
  if (name == "relay")         return CAP_RELAY;
  if (name == "i2c")           return CAP_I2C;
  if (name == "uart")          return CAP_UART;
  if (name == "irTx")          return CAP_IR_TX;
  if (name == "irRx")          return CAP_IR_RX;
  return 0;
}

// ── Capability name (lowercase string, matching hub/UI convention) ──────────
inline String capTypeName(uint8_t type) {
  switch (type) {
    case CAP_ANALOG_IN:   return "analogInput";
    case CAP_ANALOG_OUT:  return "analogOutput";
    case CAP_DIGITAL_IN:  return "digitalInput";
    case CAP_DIGITAL_OUT: return "digitalOutput";
    case CAP_RELAY:       return "relay";
    case CAP_I2C:         return "i2c";
    case CAP_UART:        return "uart";
    case CAP_IR_TX:       return "irTx";
    case CAP_IR_RX:       return "irRx";
    default:              return "unknown";
  }
}

// ── Serialize capabilities to compact string ─────────────────────────────────
// Format: capCount,type,pin,extra,label;type,pin,extra,label;...
inline String serializeCaps(const CapabilitySlot* caps, uint8_t count) {
  String s;
  for (uint8_t i = 0; i < count && i < CAP_MAX_COUNT; i++) {
    if (i > 0) s += ';';
    s += String(caps[i].type) + ',' +
         String(caps[i].pin) + ',' +
         String(caps[i].extra) + ',' +
         String(caps[i].label);
  }
  return s;
}

// ── Deserialize capabilities from compact string ────────────────────────────
// Returns number of caps parsed.
inline uint8_t deserializeCaps(CapabilitySlot* out, const String& data) {
  uint8_t count = 0;
  memset(out, 0, sizeof(CapabilitySlot) * CAP_MAX_COUNT);
  if (data.length() == 0) return 0;

  int start = 0;
  while (start < (int)data.length() && count < CAP_MAX_COUNT) {
    int semi = data.indexOf(';', start);
    if (semi < 0) semi = data.length();
    String part = data.substring(start, semi);
    start = semi + 1;
    if (part.length() == 0) continue;

    int c1 = part.indexOf(',');
    if (c1 < 0) continue;
    int c2 = part.indexOf(',', c1 + 1);
    if (c2 < 0) continue;
    int c3 = part.indexOf(',', c2 + 1);
    if (c3 < 0) continue;

    out[count].type  = (uint8_t)part.substring(0, c1).toInt();
    out[count].pin   = (uint8_t)part.substring(c1 + 1, c2).toInt();
    out[count].extra = (uint8_t)part.substring(c2 + 1, c3).toInt();
    String lbl = part.substring(c3 + 1);
    lbl.toCharArray(out[count].label, CAP_LABEL_LEN);
    count++;
  }
  return count;
}

// ── Apply pin modes for a set of capabilities ───────────────────────────────
inline void applyCapPinModes(const CapabilitySlot* caps, uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    switch (caps[i].type) {
      case CAP_ANALOG_OUT:
        pinMode(caps[i].pin, OUTPUT);
        ledcSetup(i, 5000, 8);
        ledcAttachPin(caps[i].pin, i);
        ledcWrite(i, 0);
        break;
      case CAP_DIGITAL_OUT:
      case CAP_RELAY:
        pinMode(caps[i].pin, OUTPUT);
        digitalWrite(caps[i].pin, LOW);
        break;
      case CAP_DIGITAL_IN:
        pinMode(caps[i].pin, INPUT_PULLUP);
        break;
      case CAP_ANALOG_IN:
        pinMode(caps[i].pin, INPUT);
        break;
      default:
        // I2C, UART, IR handled externally
        break;
    }
  }
}

#endif // CAPABILITIES_HPP
