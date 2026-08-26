#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "critical_alarm.h"

// /alerts.csv is append-only. Rewriting a row in place on an SD card is not crash-safe;
// appending events is, and a brownout mid-episode still leaves a readable file.

inline const char* alarmCauseName(AlarmCause cause) {
  switch (cause) {
    case AlarmCause::CONFIRMED_HIGH: return "CONFIRMED_HIGH";
    case AlarmCause::CONFIRMED_LOW: return "CONFIRMED_LOW";
    case AlarmCause::LOST_WHILE_CRITICAL: return "LOST_WHILE_CRITICAL";
    case AlarmCause::IMPLAUSIBLE_COLLAPSE: return "IMPLAUSIBLE_COLLAPSE";
    case AlarmCause::NONE: break;
  }
  return "NONE";
}

inline void formatAlertRow(char* out, size_t cap, const char* timestamp, const char* event,
                           int hr, int spo2, const char* detail) {
  if (out == nullptr || cap == 0) return;
  snprintf(out, cap, "%s,%s,%d,%d,%s", timestamp == nullptr ? "" : timestamp,
           event == nullptr ? "" : event, hr, spo2, detail == nullptr ? "" : detail);
}

struct OpenEpisode {
  bool open = false;
  bool low = false;          // the ONSET row named CONFIRMED_LOW
  uint32_t onsetEpoch = 0;
};

// Fed one line at a time in file order. The caller parses the timestamp with the sketch's
// clock helpers and passes it in, so this stays free of time.h and testable natively.
inline void reconcileAlertLine(const char* line, uint32_t lineEpoch, OpenEpisode& state) {
  if (line == nullptr) return;
  const char* firstComma = strchr(line, ',');
  if (firstComma == nullptr) return;
  const char* event = firstComma + 1;
  const char* secondComma = strchr(event, ',');
  if (secondComma == nullptr) return;
  const size_t len = static_cast<size_t>(secondComma - event);

  if (len == 5 && strncmp(event, "ONSET", 5) == 0) {
    state.open = true;
    // The cause is the detail column, so an episode resumed after a power cut can still say which
    // way the heart rate went. Getting this wrong would put "HIGH HEART RATE" on the screen for a
    // bradycardia episode, which is worse than saying nothing.
    state.low = strstr(event, "CONFIRMED_LOW") != nullptr;
    state.onsetEpoch = lineEpoch;
    return;
  }
  if (len == 7 && strncmp(event, "DISMISS", 7) == 0) {
    state.open = false;
    state.low = false;
    state.onsetEpoch = 0;
  }
  // RESOLVED and BOOT_RESUME deliberately do not change openness: the alarm latches until
  // a person dismisses it, so a rate coming back down does not close the episode.
}
