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
  uint16_t beatMs = 0;
  float skinC = 0.0f;
  bool skinValid = false;
};

inline bool decodeBandFrame(const uint8_t* bytes, size_t length, BandReading& out) {
  if (bytes == nullptr || length < 23 || bytes[0] != 0xF5 || bytes[1] != 0x03) return false;
  out.sequence = bytes[2];
  out.signal = bytes[4];
  out.heartRate = bytes[10];
  out.beatMs = static_cast<uint16_t>((static_cast<uint16_t>(bytes[11]) << 8) | bytes[12]);
  out.oxygenSaturation = bytes[13];
  out.skinC = static_cast<float>((static_cast<uint16_t>(bytes[6]) << 8) | bytes[7]) / 10.0f;
  out.skinValid = out.skinC >= BAND_SKIN_MIN_C && out.skinC <= BAND_SKIN_MAX_C;
  return true;
}

// The band will report a heart rate off bedding. Left facing a mattress in a dark room with no
// pulse anywhere near it, it produced three measurements of 90, 93 and 96 bpm - values that read
// as a calmly sleeping baby - while its own beat interval implied 136, 149 and 155. That is the
// dangerous failure: a band that has come off does not go quiet, it goes reassuring.
//
// Byte 10 and bytes 11-12 are independent enough to catch it. Worn, they agree: across 182
// measurements of a sleeping baby the mean disagreement was +0.5 bpm (sd 8.8) and not one reading
// disagreed by more than 40. Every fabric reading disagreed by 46-59. So 40 bpm separates them
// with no false positives in the data we have.
//
// This only marks the reading. It is still displayed, still logged, still fed to the alarm: a
// receiver that silently drops readings is worse than one that shows a doubtful number and says
// so. See docs/PROTOCOL.md.
constexpr int BAND_HR_DISAGREE_BPM = 40;

inline bool bandReadingDisagrees(int heartRate, int beatMs) {
  if (heartRate <= 0 || beatMs <= 0) return false;
  const float implied = 60000.0f / static_cast<float>(beatMs);
  const float hr = static_cast<float>(heartRate);
  const float diff = implied > hr ? implied - hr : hr - implied;
  return diff > static_cast<float>(BAND_HR_DISAGREE_BPM);
}

struct ReadingSnapshot {
  int sequence = -1;
  int heartRate = 0;
  int beatMs = 0;
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
  next.beatMs = frame.beatMs;
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
