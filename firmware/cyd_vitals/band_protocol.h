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
// When they disagree the beat interval is the one to trust, and the reading is corrected rather
// than merely marked. See effectiveHeartRate() below. See docs/PROTOCOL.md.
constexpr int BAND_HR_DISAGREE_BPM = 40;

inline bool bandReadingDisagrees(int heartRate, int beatMs) {
  if (heartRate <= 0 || beatMs <= 0) return false;
  const float implied = 60000.0f / static_cast<float>(beatMs);
  const float hr = static_cast<float>(heartRate);
  const float diff = implied > hr ? implied - hr : hr - implied;
  return diff > static_cast<float>(BAND_HR_DISAGREE_BPM);
}

// Byte 10 does not only invent a rate off bedding - worn, it also locks onto every second beat and
// reports half the truth. Across 33 days of logs (~115,000 readings) the rate fell to almost
// exactly half of the preceding two minutes 70 times, typically ~160 -> ~80 for 40 s to 5 minutes,
// and it never once read above 191. A band that cannot say 200 cannot raise a tachycardia alarm,
// and a halved 160 arrives as an 80 that trips the bradycardia alarm instead. Both failures are
// the same bug, and both are corrected by the same field.
//
// So when the two decodes disagree, the beat interval wins: it is a measured interval rather than
// a tracker's average, and it is what caught the bedding readings too. The interval is only
// trusted inside a range no human heart leaves. 150 ms is 400 bpm - infant SVT reaches 250-300,
// so the ceiling has to sit well above it - and 2000 ms is 30 bpm. Outside that the field is
// garbage and the raw byte is kept, doubtful but at least measured.
constexpr int BAND_BEAT_MS_MIN = 150;   // 400 bpm
constexpr int BAND_BEAT_MS_MAX = 2000;  // 30 bpm

inline bool bandBeatPlausible(int beatMs) {
  return beatMs >= BAND_BEAT_MS_MIN && beatMs <= BAND_BEAT_MS_MAX;
}

// The rate to display, plot, log and alarm on. Equal to the raw byte unless the band contradicts
// itself with a usable interval. A zero heart rate stays zero: an idle or charging band must never
// be handed a manufactured rate.
inline int effectiveHeartRate(int heartRate, int beatMs) {
  if (heartRate <= 0) return heartRate;
  if (!bandBeatPlausible(beatMs)) return heartRate;
  if (!bandReadingDisagrees(heartRate, beatMs)) return heartRate;
  return (60000 + beatMs / 2) / beatMs;
}

inline bool readingCorrected(int heartRate, int beatMs) {
  return effectiveHeartRate(heartRate, beatMs) != heartRate;
}

struct ReadingSnapshot {
  int sequence = -1;
  int heartRate = 0;            // byte 10, exactly as broadcast - the raw column in the CSV
  int beatMs = 0;
  // Derived once here so the display, the plots, both alarms and the log cannot disagree about
  // what the rate was. Everything downstream reads these two rather than recomputing.
  int effectiveHeartRate = 0;
  bool heartRateCorrected = false;
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
  next.effectiveHeartRate = effectiveHeartRate(frame.heartRate, frame.beatMs);
  next.heartRateCorrected = next.effectiveHeartRate != frame.heartRate;
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
