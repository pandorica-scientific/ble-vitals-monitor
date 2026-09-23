#pragma once

#include <stddef.h>
#include <stdint.h>

// The wristband's 23-byte advertisement and the rules for turning it into a heart rate. Pure logic.
// Every number below is justified in docs/PROTOCOL.md; this file states the rules, not the evidence.

constexpr size_t BAND_FRAME_LEN = 23;
constexpr uint8_t BAND_FRAME_MARKER = 0xF5;
constexpr uint8_t BAND_DEVICE_WRISTBAND = 0x03;

// Skin temperature outside this range is treated as no reading. An unworn band in a warm room can
// still read 28.1 C, so this is a sanity gate, not a wear detector.
constexpr float BAND_SKIN_MIN_C = 28.0f;
constexpr float BAND_SKIN_MAX_C = 42.0f;

struct BandReading {
  uint8_t sequence = 0;          // byte 2: measurement counter, +1 per new reading (~20 s)
  uint8_t signal = 0;            // byte 4: signal-quality hint
  uint8_t heartRate = 0;         // byte 10: bpm, a windowed average
  uint8_t oxygenSaturation = 0;  // byte 13: percent
  uint16_t beatMs = 0;           // bytes 11-12, big-endian: a recent single-beat interval
  float skinC = 0.0f;            // bytes 6-7, big-endian, tenths of a degree
  bool skinValid = false;
};

inline bool decodeBandFrame(const uint8_t* bytes, size_t length, BandReading& out) {
  if (bytes == nullptr || length < BAND_FRAME_LEN) return false;
  if (bytes[0] != BAND_FRAME_MARKER || bytes[1] != BAND_DEVICE_WRISTBAND) return false;
  out.sequence = bytes[2];
  out.signal = bytes[4];
  out.heartRate = bytes[10];
  out.beatMs = static_cast<uint16_t>((static_cast<uint16_t>(bytes[11]) << 8) | bytes[12]);
  out.oxygenSaturation = bytes[13];
  out.skinC = static_cast<float>((static_cast<uint16_t>(bytes[6]) << 8) | bytes[7]) / 10.0f;
  out.skinValid = out.skinC >= BAND_SKIN_MIN_C && out.skinC <= BAND_SKIN_MAX_C;
  return true;
}

// ---- heart-rate validity ------------------------------------------------------------------------
//
// The band reports a heart rate off bedding: facing a mattress it produced 90-96 bpm while its own
// beat interval implied 136-155. Worn, the two decodes agree to within 40 bpm in every genuine
// reading captured; off the wrist they disagreed by 46-59. So a disagreement above 40 bpm means one
// of the two decodes has lost the beat. Which one is settled against the recent past, below.

constexpr int BAND_HR_DISAGREE_BPM = 40;

inline bool bandReadingDisagrees(int heartRate, int beatMs) {
  if (heartRate <= 0 || beatMs <= 0) return false;
  const float implied = 60000.0f / static_cast<float>(beatMs);
  const float hr = static_cast<float>(heartRate);
  const float diff = implied > hr ? implied - hr : hr - implied;
  return diff > static_cast<float>(BAND_HR_DISAGREE_BPM);
}

// The interval is only believed inside a range no human heart leaves: 150 ms is 400 bpm (infant SVT
// reaches 250-300, so the ceiling sits well above it) and 2000 ms is 30 bpm.
constexpr int BAND_BEAT_MS_MIN = 150;
constexpr int BAND_BEAT_MS_MAX = 2000;

inline bool bandBeatPlausible(int beatMs) {
  return beatMs >= BAND_BEAT_MS_MIN && beatMs <= BAND_BEAT_MS_MAX;
}

// The rate the interval implies, rounded to nearest, or 0 when the interval is unusable.
inline int intervalHeartRate(int beatMs) {
  if (!bandBeatPlausible(beatMs)) return 0;
  return (60000 + beatMs / 2) / beatMs;
}

// ---- rate context -------------------------------------------------------------------------------
//
// Both decodes lock onto every second beat now and then, independently of each other: over 18 days
// of logs the byte halved 368 times and the interval doubled 389 times. What separates them in every
// case is the recent past - the one that lost the beat sits at half or double the rate of the last
// few minutes. So the last five minutes of readings on which the two agreed are kept, and a
// disagreement is judged against their median.

constexpr int RATE_CONTEXT_CAP = 16;             // five minutes at one measurement every ~20 s
constexpr uint32_t RATE_CONTEXT_MS = 300'000;
constexpr int RATE_CONTEXT_MIN = 3;              // fewer than this is not a trend

struct RateContext {
  int hr[RATE_CONTEXT_CAP] = {};
  uint32_t ms[RATE_CONTEXT_CAP] = {};
  int head = 0;
  int count = 0;
};

inline void rateContextNote(RateContext& c, int heartRate, uint32_t nowMs) {
  c.hr[c.head] = heartRate;
  c.ms[c.head] = nowMs;
  c.head = (c.head + 1) % RATE_CONTEXT_CAP;
  if (c.count < RATE_CONTEXT_CAP) ++c.count;
}

// Median of the readings noted within the last five minutes. False when there are fewer than
// RATE_CONTEXT_MIN of them, so a lone reading after a dropout cannot pose as a trend.
inline bool rateContextReference(const RateContext& c, uint32_t nowMs, int& out) {
  int recent[RATE_CONTEXT_CAP];
  int n = 0;
  for (int i = 0; i < c.count; ++i) {
    if (static_cast<uint32_t>(nowMs - c.ms[i]) <= RATE_CONTEXT_MS) recent[n++] = c.hr[i];
  }
  if (n < RATE_CONTEXT_MIN) return false;
  for (int i = 1; i < n; ++i) {   // insertion sort: n is at most sixteen
    const int v = recent[i];
    int j = i - 1;
    while (j >= 0 && recent[j] > v) {
      recent[j + 1] = recent[j];
      --j;
    }
    recent[j + 1] = v;
  }
  out = (n % 2) ? recent[n / 2] : (recent[n / 2 - 1] + recent[n / 2]) / 2;
  return true;
}

// The rate to display, plot, log and feed the slow alarm. Equal to the raw byte unless the band
// contradicts itself with a usable interval AND the interval is the candidate continuous with the
// recent past. A zero heart rate stays zero: an idle or charging band is never handed a manufactured
// rate. With no reference yet, or on a tie, the byte stands as the decode one step closer to the
// sensor.
inline int resolveHeartRate(int heartRate, int beatMs, bool haveReference, int reference) {
  if (heartRate <= 0) return heartRate;
  if (!bandBeatPlausible(beatMs)) return heartRate;
  if (!bandReadingDisagrees(heartRate, beatMs)) return heartRate;
  if (!haveReference) return heartRate;
  const int implied = intervalHeartRate(beatMs);
  const int impliedGap = implied > reference ? implied - reference : reference - implied;
  const int byteGap = heartRate > reference ? heartRate - reference : reference - heartRate;
  return impliedGap < byteGap ? implied : heartRate;
}

// What the fast alarm hears. Continuity has one blind spot: a tachycardia that begins abruptly (as
// supraventricular tachycardia does) while the byte halves at the same moment - the halved byte is
// then the candidate nearer the recent past. So the fast alarm is handed the faster decode whenever
// the band contradicts itself, flagged as corrected so the longer confirmation window applies. The
// slow alarm hears the displayed rate only: a doubled interval must never pose as bradycardia.
inline int highAlarmHeartRate(int heartRate, int beatMs, int effectiveHeartRate) {
  if (heartRate <= 0) return effectiveHeartRate;
  if (!bandBeatPlausible(beatMs)) return effectiveHeartRate;
  if (!bandReadingDisagrees(heartRate, beatMs)) return effectiveHeartRate;
  const int implied = intervalHeartRate(beatMs);
  return implied > effectiveHeartRate ? implied : effectiveHeartRate;
}

// ---- the published reading ----------------------------------------------------------------------

struct ReadingSnapshot {
  int sequence = -1;
  int heartRate = 0;   // byte 10 exactly as broadcast: the raw column in the CSV
  int beatMs = 0;
  // Derived once per measurement so the display, the plots, both alarms and the log cannot disagree
  // about what the rate was.
  int effectiveHeartRate = 0;        // shown, plotted, logged, and heard by the slow alarm
  bool heartRateCorrected = false;   // effectiveHeartRate came from the beat interval
  int highAlarmHeartRate = 0;        // heard by the fast alarm: the faster decode when they disagree
  int oxygenSaturation = 0;
  int signal = 0;
  int rssi = 0;
  float skinC = 0.0f;
  bool skinValid = false;
  uint32_t lastPacketMs = 0;
};

// The band rebroadcasts one measurement for ~20 s. The choice between the two decodes is made once,
// on the first frame of a new sequence, and held: otherwise a context entry expiring mid-sequence
// could flip the number on screen between two copies of the same reading.
inline ReadingSnapshot mergeBandReading(const ReadingSnapshot& previous, const BandReading& frame,
                                        int rssi, uint32_t nowMs, RateContext& context) {
  ReadingSnapshot next = previous;
  const bool newMeasurement = previous.lastPacketMs == 0 || frame.sequence != previous.sequence;
  next.sequence = frame.sequence;
  next.heartRate = frame.heartRate;
  next.beatMs = frame.beatMs;
  if (newMeasurement) {
    int reference = 0;
    const bool haveReference = rateContextReference(context, nowMs, reference);
    next.effectiveHeartRate =
        resolveHeartRate(frame.heartRate, frame.beatMs, haveReference, reference);
    next.heartRateCorrected = next.effectiveHeartRate != frame.heartRate;
    next.highAlarmHeartRate =
        highAlarmHeartRate(frame.heartRate, frame.beatMs, next.effectiveHeartRate);
    if (frame.heartRate > 0 && !bandReadingDisagrees(frame.heartRate, frame.beatMs)) {
      rateContextNote(context, frame.heartRate, nowMs);
    }
  }
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
