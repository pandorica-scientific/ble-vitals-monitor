#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr float BAND_SKIN_MIN_C = 28.0f;
constexpr float BAND_SKIN_MAX_C = 42.0f;

struct BandReading {
  uint8_t sequence = 0;
  uint8_t signal = 0;
  uint8_t heartRate = 0;
  uint8_t oxygenSaturation = 0;
  float skinC = 0.0f;
  bool skinValid = false;
};

inline bool decodeBandFrame(const uint8_t* bytes, size_t length, BandReading& out) {
  if (bytes == nullptr || length < 23 || bytes[0] != 0xF5 || bytes[1] != 0x03) return false;
  out.sequence = bytes[2];
  out.signal = bytes[4];
  out.heartRate = bytes[10];
  out.oxygenSaturation = bytes[13];
  out.skinC = static_cast<float>((static_cast<uint16_t>(bytes[6]) << 8) | bytes[7]) / 10.0f;
  out.skinValid = out.skinC >= BAND_SKIN_MIN_C && out.skinC <= BAND_SKIN_MAX_C;
  return true;
}

struct ReadingSnapshot {
  int sequence = -1;
  int heartRate = 0;
  int oxygenSaturation = 0;
  int signal = 0;
  int rssi = 0;
  float skinC = 0.0f;
  bool skinValid = false;
  uint32_t lastPacketMs = 0;
};

inline ReadingSnapshot mergeBandReading(const ReadingSnapshot& previous, const BandReading& frame,
                                        int rssi, uint32_t nowMs) {
  ReadingSnapshot next = previous;
  next.sequence = frame.sequence;
  next.heartRate = frame.heartRate;
  next.oxygenSaturation = frame.oxygenSaturation;
  next.signal = frame.signal;
  next.rssi = rssi;
  if (frame.skinValid) {
    next.skinC = frame.skinC;
    next.skinValid = true;
  }
  next.lastPacketMs = nowMs;
  return next;
}
