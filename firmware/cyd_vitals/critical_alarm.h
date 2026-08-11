#pragma once

#include <stdint.h>

// Critical tachycardia alarm. Pure logic: no Arduino, no display, no SD.
//
// The alarm exists for one situation - a sustained heart rate at or above 200 bpm, which
// carries a roughly ten-minute window before a hospital visit. Everything here is sized
// against that number: a one-minute confirmation costs 10% of it, and the alarm latches
// because an episode that is missed by looking away is an episode that did not exist.

enum class AlarmState : uint8_t { IDLE, ARMED, ALARM };

enum class AlarmCause : uint8_t {
  NONE,
  CONFIRMED_HIGH,        // the confirmation window closed with the criteria met
  LOST_WHILE_CRITICAL,   // signal died while armed - silence is not a drop
  IMPLAUSIBLE_COLLAPSE,  // a high rate became a very low one in one step
};

struct AlarmConfig {
  int crit = 200;          // arms the window; readings at or above count toward minHigh
  int sustain = 190;       // the last reading must be at or above this to confirm
  int cancel = 180;        // a reading below this abandons the window outright
  int collapseFrom = 170;  // previous reading at or above this...
  int collapseTo = 60;     // ...and this reading at or below it escalates immediately
  int minHigh = 2;         // readings at or above crit needed within one window
  uint32_t confirmMs = 60'000;
  uint32_t snoozeMs = 600'000;
};

// Plain-old-data so the whole thing can be copied into RTC memory across a soft reboot.
struct AlarmMachine {
  AlarmState state = AlarmState::IDLE;
  AlarmCause cause = AlarmCause::NONE;
  uint32_t onsetEpoch = 0;
  uint32_t resolvedEpoch = 0;
  bool resolved = false;
  int peak = 0;

  int lastHr = 0;      // last valid reading in any state, for collapse detection
  bool haveLast = false;

  uint32_t windowStartMs = 0;
  int windowReadings = 0;
  int windowHighCount = 0;
  int windowLastHr = 0;
  bool windowResolving = true;  // strictly decreasing so far

  uint32_t dismissedMs = 0;
  bool everDismissed = false;
};

struct AlarmEvent {
  bool alarmStarted = false;
  bool episodeResolved = false;
};

inline bool alarmSnoozed(const AlarmMachine& m, const AlarmConfig& cfg, uint32_t nowMs) {
  if (!m.everDismissed) return false;
  return static_cast<uint32_t>(nowMs - m.dismissedMs) < cfg.snoozeMs;
}

inline void alarmResetWindow(AlarmMachine& m, uint32_t nowMs, int seedHr) {
  m.windowStartMs = nowMs;
  m.windowReadings = 1;
  m.windowHighCount = 0;
  m.windowLastHr = seedHr;
  m.windowResolving = true;
}

inline void alarmAccumulate(AlarmMachine& m, const AlarmConfig& cfg, int hr) {
  // Resolution means strictly decreasing. Holding level breaks it exactly as a rise does:
  // a rate that has stopped falling has stopped recovering, however it got there.
  if (m.windowReadings > 0 && hr >= m.windowLastHr) m.windowResolving = false;
  ++m.windowReadings;
  if (hr >= cfg.crit) ++m.windowHighCount;
  m.windowLastHr = hr;
}

inline void alarmEnter(AlarmMachine& m, AlarmCause cause, uint32_t onsetEpoch, AlarmEvent& out) {
  m.state = AlarmState::ALARM;
  m.cause = cause;
  m.onsetEpoch = onsetEpoch;
  m.resolved = false;
  m.resolvedEpoch = 0;
  out.alarmStarted = true;
}

inline void alarmToIdle(AlarmMachine& m) {
  m.state = AlarmState::IDLE;
  m.cause = AlarmCause::NONE;
  m.onsetEpoch = 0;
  m.peak = 0;
  m.windowReadings = 0;
  m.windowHighCount = 0;
  m.windowLastHr = 0;
  m.windowResolving = true;
  m.resolved = false;
  m.resolvedEpoch = 0;
}

inline AlarmEvent alarmOnReading(AlarmMachine& m, const AlarmConfig& cfg, int hr,
                                 uint32_t nowMs, uint32_t nowEpoch) {
  AlarmEvent event{};
  const int previous = m.lastHr;
  const bool hadPrevious = m.haveLast;
  m.lastHr = hr;
  m.haveLast = true;

  if (m.state == AlarmState::ALARM) {
    if (hr > m.peak) m.peak = hr;
    if (hr > 0 && hr < cfg.crit && !m.resolved) {
      m.resolved = true;
      m.resolvedEpoch = nowEpoch;
      event.episodeResolved = true;
    }
    return event;
  }

  // A step from a high rate to a very low one is either the uint8_t wrapping past 255 or a
  // genuine catastrophic event. We cannot tell them apart and do not need to: both alarm.
  if (hadPrevious && previous >= cfg.collapseFrom && hr > 0 && hr <= cfg.collapseTo) {
    if (!alarmSnoozed(m, cfg, nowMs)) {
      m.peak = previous > hr ? previous : hr;
      alarmEnter(m, AlarmCause::IMPLAUSIBLE_COLLAPSE, nowEpoch, event);
    }
    return event;
  }

  if (m.state == AlarmState::IDLE) {
    if (hr >= cfg.crit) {
      m.state = AlarmState::ARMED;
      m.onsetEpoch = nowEpoch;
      m.peak = hr;
      alarmResetWindow(m, nowMs, hr);
      m.windowHighCount = 1;
    }
    return event;
  }

  // ARMED
  if (hr > 0 && hr < cfg.cancel) {
    alarmToIdle(m);
    return event;
  }
  if (hr > m.peak) m.peak = hr;
  alarmAccumulate(m, cfg, hr);
  return event;
}

// No epoch parameter: both alarm entries here take their onset from candidateOnset, which was
// stamped when the rate first crossed. Passing the current time would only invite using it.
inline AlarmEvent alarmTick(AlarmMachine& m, const AlarmConfig& cfg, uint32_t nowMs, bool stale) {
  AlarmEvent event{};
  if (m.state != AlarmState::ARMED) return event;

  if (stale) {
    // The last thing this saw was a critical rate, and then it saw nothing. Silence is not
    // a drop, and the window cannot be completed without data.
    if (!alarmSnoozed(m, cfg, nowMs)) {
      alarmEnter(m, AlarmCause::LOST_WHILE_CRITICAL, m.onsetEpoch, event);
    }
    return event;
  }

  if (static_cast<uint32_t>(nowMs - m.windowStartMs) < cfg.confirmMs) return event;

  // A trend cannot be established from one point, and that point was at or above crit,
  // so the unreachable single-reading case fails toward alarming.
  const bool notResolving = m.windowReadings < 2 || !m.windowResolving;
  const bool confirmed = m.windowLastHr >= cfg.sustain &&
                         (m.windowHighCount >= cfg.minHigh || notResolving);

  if (confirmed && !alarmSnoozed(m, cfg, nowMs)) {
    alarmEnter(m, AlarmCause::CONFIRMED_HIGH, m.onsetEpoch, event);
    return event;
  }

  // Not confirmed: start a fresh window without touching onsetEpoch. Resetting to IDLE here
  // would let a rate oscillating either side of crit run indefinitely without ever alarming,
  // because every window would expire just short and start again from nothing.
  const int carried = m.windowLastHr;
  alarmResetWindow(m, nowMs, carried);
  if (carried >= cfg.crit) m.windowHighCount = 1;
  return event;
}

inline void alarmDismiss(AlarmMachine& m, uint32_t nowMs) {
  alarmToIdle(m);
  m.dismissedMs = nowMs;
  m.everDismissed = true;
}

inline uint32_t alarmElapsedS(const AlarmMachine& m, uint32_t nowEpoch) {
  if (m.onsetEpoch == 0 || nowEpoch < m.onsetEpoch) return 0;
  return nowEpoch - m.onsetEpoch;
}
