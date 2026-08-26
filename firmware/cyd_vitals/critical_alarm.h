#pragma once

#include <stdint.h>

// Critical heart-rate alarm, both directions. Pure logic: no Arduino, no display, no SD.
//
// The alarm exists for one situation - a sustained heart rate far enough outside the safe band to
// carry a roughly ten-minute window before a hospital visit. Everything here is sized against
// that number: a one-minute confirmation costs 10% of it, and the alarm latches because an
// episode that is missed by looking away is an episode that did not exist.
//
// Bradycardia runs the SAME machine with its thresholds read the other way up, set by
// AlarmConfig::lowSide. Every comparison against a threshold goes through alarmBeyond(), so there
// is one confirmation window, one latch and one snooze to reason about rather than two copies
// that can drift apart. Two differences are deliberate, not oversights:
//
//   - A zero heart rate is thrown away on the low side before it touches the machine. An idle or
//     charging band broadcasts hr = 0, and zero is "no reading", not "a very slow heart". On the
//     high side this never mattered, because zero is nowhere near 200. Here it is everything.
//   - The implausible-collapse rule stays high-side only. It exists because a fall from a high
//     rate to a very low one is either the uint8_t wrapping past 255 or a catastrophe, and both
//     deserve an alarm. The mirror image - a very low rate jumping to a very high one - has no
//     such reading; against a wristband that manufactures numbers off bedding it is far more
//     likely to be an artifact, and alarming on it would wake the house for nothing.

enum class AlarmState : uint8_t { IDLE, ARMED, ALARM };

// Appended to, never reordered: an AlarmMachine survives a soft reboot in RTC memory, so the
// numbering has to stay stable across a firmware update.
enum class AlarmCause : uint8_t {
  NONE,
  CONFIRMED_HIGH,        // the confirmation window closed with the criteria met
  LOST_WHILE_CRITICAL,   // signal died while armed - silence is not a drop
  IMPLAUSIBLE_COLLAPSE,  // a high rate became a very low one in one step
  CONFIRMED_LOW,         // the same window, closed on the low side
};

// "Beyond" a threshold means at or above it on the high side, at or below it on the low side.
// Every comparison below is written that way round, so the same numbers describe both alarms.
struct AlarmConfig {
  int crit = 200;          // arms the window; readings beyond it count toward minCritical
  int sustain = 190;       // the last reading must be beyond this to confirm
  int cancel = 180;        // a reading short of this abandons the window outright
  int collapseFrom = 170;  // previous reading at or above this...
  int collapseTo = 60;     // ...and this reading at or below it escalates immediately
  int minCritical = 2;     // readings beyond crit needed within one window
  uint32_t confirmMs = 60'000;
  uint32_t snoozeMs = 600'000;
  bool lowSide = false;    // read every threshold the other way up
};

inline bool alarmBeyond(const AlarmConfig& cfg, int hr, int threshold) {
  return cfg.lowSide ? hr <= threshold : hr >= threshold;
}

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
  int windowCritCount = 0;
  int windowLastHr = 0;
  bool windowResolving = true;  // moving strictly back toward the safe band so far

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
  m.windowCritCount = 0;
  m.windowLastHr = seedHr;
  m.windowResolving = true;
}

inline void alarmAccumulate(AlarmMachine& m, const AlarmConfig& cfg, int hr) {
  // Resolution means moving strictly back toward the safe band. Holding level breaks it exactly
  // as moving the wrong way does: a rate that has stopped recovering has stopped recovering,
  // however it got there.
  const bool notRecovering = cfg.lowSide ? hr <= m.windowLastHr : hr >= m.windowLastHr;
  if (m.windowReadings > 0 && notRecovering) m.windowResolving = false;
  ++m.windowReadings;
  if (alarmBeyond(cfg, hr, cfg.crit)) ++m.windowCritCount;
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
  m.windowCritCount = 0;
  m.windowLastHr = 0;
  m.windowResolving = true;
  m.resolved = false;
  m.resolvedEpoch = 0;
}

inline AlarmEvent alarmOnReading(AlarmMachine& m, const AlarmConfig& cfg, int hr,
                                 uint32_t nowMs, uint32_t nowEpoch) {
  AlarmEvent event{};
  // Zero is the absence of a reading. Dropped here, before it can arm anything, seed a window or
  // pass for a recovery - staleness, not this, is what escalates a band that has gone quiet.
  if (cfg.lowSide && hr <= 0) return event;

  const int previous = m.lastHr;
  const bool hadPrevious = m.haveLast;
  m.lastHr = hr;
  m.haveLast = true;

  if (m.state == AlarmState::ALARM) {
    // "peak" is the most extreme reading of the episode, which on the low side is the lowest.
    if (cfg.lowSide ? (hr < m.peak) : (hr > m.peak)) m.peak = hr;
    if (hr > 0 && !alarmBeyond(cfg, hr, cfg.crit) && !m.resolved) {
      m.resolved = true;
      m.resolvedEpoch = nowEpoch;
      event.episodeResolved = true;
    }
    return event;
  }

  // A step from a high rate to a very low one is either the uint8_t wrapping past 255 or a
  // genuine catastrophic event. We cannot tell them apart and do not need to: both alarm.
  if (!cfg.lowSide && hadPrevious && previous >= cfg.collapseFrom &&
      hr > 0 && hr <= cfg.collapseTo) {
    if (!alarmSnoozed(m, cfg, nowMs)) {
      m.peak = previous > hr ? previous : hr;
      alarmEnter(m, AlarmCause::IMPLAUSIBLE_COLLAPSE, nowEpoch, event);
    }
    return event;
  }

  if (m.state == AlarmState::IDLE) {
    if (alarmBeyond(cfg, hr, cfg.crit)) {
      m.state = AlarmState::ARMED;
      m.onsetEpoch = nowEpoch;
      m.peak = hr;
      alarmResetWindow(m, nowMs, hr);
      m.windowCritCount = 1;
    }
    return event;
  }

  // ARMED
  if (hr > 0 && !alarmBeyond(cfg, hr, cfg.cancel)) {
    alarmToIdle(m);
    return event;
  }
  if (cfg.lowSide ? (hr < m.peak) : (hr > m.peak)) m.peak = hr;
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
  const bool confirmed = alarmBeyond(cfg, m.windowLastHr, cfg.sustain) &&
                         (m.windowCritCount >= cfg.minCritical || notResolving);

  if (confirmed && !alarmSnoozed(m, cfg, nowMs)) {
    alarmEnter(m, cfg.lowSide ? AlarmCause::CONFIRMED_LOW : AlarmCause::CONFIRMED_HIGH,
               m.onsetEpoch, event);
    return event;
  }

  // Not confirmed: start a fresh window without touching onsetEpoch. Resetting to IDLE here
  // would let a rate oscillating either side of crit run indefinitely without ever alarming,
  // because every window would expire just short and start again from nothing.
  const int carried = m.windowLastHr;
  alarmResetWindow(m, nowMs, carried);
  if (alarmBeyond(cfg, carried, cfg.crit)) m.windowCritCount = 1;
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
