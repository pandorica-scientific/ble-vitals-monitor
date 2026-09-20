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
// A disagreement only says that one of the two decodes has lost the beat. Which one is settled
// below, against the readings that came before. See docs/PROTOCOL.md.
constexpr int BAND_HR_DISAGREE_BPM = 40;

inline bool bandReadingDisagrees(int heartRate, int beatMs) {
  if (heartRate <= 0 || beatMs <= 0) return false;
  const float implied = 60000.0f / static_cast<float>(beatMs);
  const float hr = static_cast<float>(heartRate);
  const float diff = implied > hr ? implied - hr : hr - implied;
  return diff > static_cast<float>(BAND_HR_DISAGREE_BPM);
}

// Both decodes lock onto every second beat, independently of each other. Across 18 days of the
// board's own six-column logs (64,633 readings, 2026-09-02 to 2026-09-20) the two disagreed by
// more than 40 bpm 757 times. 368 times the byte was the one that had halved - typically ~160 on
// the interval against ~80 on the byte, for 40 s to 9 min, mostly at rates above 150 - and 389
// times the interval had doubled instead, the byte holding ~130 while the interval implied 60-76
// for up to 14 min at a stretch. Neither decode is the one to trust. The interval also throws a
// single wild reading (280, 305 or 365 ms) about every fifteen minutes, in step with the band's
// SpO2 measurement.
//
// What separates the two in every one of those cases is the recent past: the decode that has
// lost the beat sits at half or double the rate of the last few minutes, and the other one is
// continuous with it. So when they disagree, the candidate nearer the median of the last five
// minutes of agreeing readings is shown. Replayed over those 18 days this very code lands within
// 25 bpm of the surrounding readings in 80% of disagreements, against 22% for always taking the
// interval and 70% for never correcting. With no context - the first minute after boot, or after
// a long dropout - the byte stands, being the decode one step closer to the sensor. What defeats
// it is a long messy stretch where both decodes drop beats in turn, so that the context itself
// fills with halved readings; 2026-09-19 18:30-19:20 is the example, and the residue of that 20%.
//
// The interval is only considered inside a range no human heart leaves. 150 ms is 400 bpm -
// infant SVT reaches 250-300, so the ceiling has to sit well above it - and 2000 ms is 30 bpm.
// Outside that the field is garbage and the raw byte is kept, doubtful but at least measured.
constexpr int BAND_BEAT_MS_MIN = 150;   // 400 bpm
constexpr int BAND_BEAT_MS_MAX = 2000;  // 30 bpm

inline bool bandBeatPlausible(int beatMs) {
  return beatMs >= BAND_BEAT_MS_MIN && beatMs <= BAND_BEAT_MS_MAX;
}

// The rate the interval implies, rounded to nearest, or 0 when the interval is unusable.
inline int intervalHeartRate(int beatMs) {
  if (!bandBeatPlausible(beatMs)) return 0;
  return (60000 + beatMs / 2) / beatMs;
}

// The last few minutes of readings on which the two decodes agreed: what a disagreement is
// judged against. Sixteen slots cover five minutes at one measurement every ~20 s. The band
// rebroadcasts each measurement for ~20 s, and mergeBandReading() notes it once.
constexpr int RATE_CONTEXT_CAP = 16;
constexpr uint32_t RATE_CONTEXT_MS = 300000;   // five minutes
constexpr int RATE_CONTEXT_MIN = 3;            // fewer than this is not a trend

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
  for (int i = 1; i < n; ++i) {            // insertion sort: n is at most sixteen
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

// The rate to display, plot, log and feed the low alarm. Equal to the raw byte unless the band
// contradicts itself with a usable interval AND the interval is the candidate continuous with
// the recent past. A zero heart rate stays zero: an idle or charging band must never be handed a
// manufactured rate. A tie goes to the byte.
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

// What the HIGH alarm hears. The one case continuity gets wrong is a tachycardia that begins
// abruptly - which is how supraventricular tachycardia begins - while the byte halves at the same
// moment: the halved byte is then the candidate nearer the recent past, and the screen keeps it.
// So the alarm is handed the faster decode whenever the band contradicts itself, flagged as
// corrected so the longer confirmation window applies. A single wild interval arms a window that
// the next ordinary reading abandons. The low alarm hears the displayed rate only: a doubled
// interval is never allowed to pose as bradycardia.
inline int highAlarmHeartRate(int heartRate, int beatMs, int effectiveHeartRate) {
  if (heartRate <= 0) return effectiveHeartRate;
  if (!bandBeatPlausible(beatMs)) return effectiveHeartRate;
  if (!bandReadingDisagrees(heartRate, beatMs)) return effectiveHeartRate;
  const int implied = intervalHeartRate(beatMs);
  return implied > effectiveHeartRate ? implied : effectiveHeartRate;
}

struct ReadingSnapshot {
  int sequence = -1;
  int heartRate = 0;            // byte 10, exactly as broadcast - the raw column in the CSV
  int beatMs = 0;
  // Derived once per measurement here so the display, the plots, both alarms and the log cannot
  // disagree about what the rate was. Everything downstream reads these rather than recomputing.
  int effectiveHeartRate = 0;        // shown, plotted, logged, and heard by the low alarm
  bool heartRateCorrected = false;   // effectiveHeartRate came from the beat interval
  int highAlarmHeartRate = 0;        // heard by the high alarm: the faster decode when they disagree
  int oxygenSaturation = 0;
  int signal = 0;
  int rssi = 0;
  float skinC = 0.0f;
  bool skinValid = false;
  uint32_t lastPacketMs = 0;
};

inline ReadingSnapshot mergeBandReading(const ReadingSnapshot& previous, const BandReading& frame,
                                        int rssi, uint32_t nowMs, RateContext& context) {
  ReadingSnapshot next = previous;
  // The band rebroadcasts one measurement for ~20 s. The choice between the two decodes is made
  // once, on the first frame of a new sequence, and held: otherwise a context entry expiring
  // mid-sequence could flip the number on screen between two copies of the same reading.
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
