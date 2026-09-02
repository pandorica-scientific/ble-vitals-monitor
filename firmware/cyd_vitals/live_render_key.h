#pragma once

#include <stdint.h>

#include "band_protocol.h"
#include "radio_health.h"

enum AlertMask : uint8_t { ALERT_NONE = 0, ALERT_HR_LOW = 1, ALERT_HR_HIGH = 2, ALERT_SPO2_LOW = 4 };

inline uint8_t makeAlertMask(int heartRate, int oxygen, bool stale,
                             int hrLow, int hrHigh, int oxygenLow) {
  if (stale) return ALERT_NONE;
  uint8_t mask = ALERT_NONE;
  if (heartRate > 0 && heartRate < hrLow) mask |= ALERT_HR_LOW;
  if (heartRate > hrHigh) mask |= ALERT_HR_HIGH;
  if (oxygen > 0 && oxygen < oxygenLow) mask |= ALERT_SPO2_LOW;
  return mask;
}

struct LiveRenderKey {
  bool stale = true;
  int sequence = -1;
  int heartRate = 0;              // raw byte - still shown, in small type, when corrected
  int effectiveHeartRate = 0;     // what the big number reads
  int oxygenSaturation = 0;
  int signal = 0;
  int skinTenths = 0;
  bool skinValid = false;
  RadioState radioState = RadioState::STARTING;
  uint8_t alertMask = ALERT_NONE;
  int minuteKey = -1;
  bool corrected = false;         // the rate came from the beat interval
  bool readingIssue = false;      // the two decodes disagree but the interval is unusable
};

inline LiveRenderKey makeLiveRenderKey(const ReadingSnapshot& reading, RadioState radioState,
                                       bool stale, uint8_t alertMask, int minuteKey) {
  const bool corrected = !stale && reading.heartRateCorrected;
  // A disagreement the interval cannot resolve is still worth saying out loud; it is the bedding
  // case with a garbage interval, and the number on screen is the doubtful raw byte.
  const bool issue = !stale && !corrected &&
                     bandReadingDisagrees(reading.heartRate, reading.beatMs);
  return {stale, reading.sequence, reading.heartRate, reading.effectiveHeartRate,
          reading.oxygenSaturation,
          reading.signal, static_cast<int>(reading.skinC * 10.0f + 0.5f),
          reading.skinValid, radioState, alertMask, minuteKey, corrected, issue};
}

inline bool operator==(const LiveRenderKey& a, const LiveRenderKey& b) {
  return a.stale == b.stale && a.sequence == b.sequence && a.heartRate == b.heartRate &&
         a.effectiveHeartRate == b.effectiveHeartRate &&
         a.oxygenSaturation == b.oxygenSaturation && a.signal == b.signal &&
         a.skinTenths == b.skinTenths && a.skinValid == b.skinValid &&
         a.radioState == b.radioState && a.alertMask == b.alertMask &&
         a.minuteKey == b.minuteKey && a.corrected == b.corrected &&
         a.readingIssue == b.readingIssue;
}

inline bool operator!=(const LiveRenderKey& a, const LiveRenderKey& b) { return !(a == b); }
