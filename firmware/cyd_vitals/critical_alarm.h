#pragma once

#include <stdint.h>

// Critical heart-rate alarm, both directions. Pure logic: no Arduino, no display, no SD.
//
// The alarm exists for one situation: a sustained heart rate far enough outside the safe band to
// carry a roughly ten-minute window before a hospital visit. A one-minute confirmation costs a tenth
// of that window, and the alarm latches because an episode missed by looking away is an episode that
// did not exist.
//
// Bradycardia runs the SAME machine with its thresholds read the other way up (AlarmConfig::lowSide).
// Every comparison goes through alarmBeyond(), so there is one confirmation window, one latch and
// one snooze rather than two copies that can drift apart. Two asymmetries are deliberate:
//
//   - A zero heart rate is dropped on the low side before it touches the machine. An idle or
//     charging band broadcasts hr = 0, and zero is "no reading", not "a very slow heart".
//   - The implausible-collapse rule is high-side only. A fall from a high rate to a very low one is
//     either the uint8_t wrapping past 255 or a catastrophe, and both deserve an alarm. Its mirror
//     image, a very low rate jumping to a very high one, is far more likely an artifact from a band
//     that manufactures numbers off bedding.
//
// A reading can arrive CORRECTED: the rate came from the band's beat interval rather than its byte
// (see band_protocol.h). That is one step further from the sensor, so a window holding any corrected
// reading is held to a higher bar - three critical readings across 120 s instead of two across 60 s -
// and a corrected reading can never fire the collapse rule, because a single long interval would
// turn a normal rate into an instant "collapse" with no window to catch it.

enum class AlarmState : uint8_t { IDLE, ARMED, ALARM };

// Appended to, never reordered: an AlarmMachine survives a soft reboot in RTC memory, so the
// numbering has to stay stable across a firmware update.
enum class AlarmCause : uint8_t {
  NONE,
  CONFIRMED_HIGH,        // the confirmation window closed with the criteria met
  LOST_WHILE_CRITICAL,   // signal died while armed: silence is not a drop
  IMPLAUSIBLE_COLLAPSE,  // a high rate became a very low one in one step
  CONFIRMED_LOW,         // the same window, closed on the low side
  CONFIRMED_HIGH_BEAT,   // confirmed high, but the window rested on beat-interval readings
};

// "Beyond" a threshold means at or above it on the high side, at or below it on the low side.
struct AlarmConfig {
  int crit = 200;          // arms the window; readings beyond it count toward minCritical
  int sustain = 190;       // the last reading must be beyond this to confirm
  int cancel = 180;        // a reading short of this abandons the window outright
  int collapseFrom = 170;  // previous reading at or above this ...
  int collapseTo = 60;     // ... and this reading at or below it escalates immediately
  int minCritical = 2;     // readings beyond crit needed within one window
  uint32_t confirmMs = 60'000;
  uint32_t snoozeMs = 600'000;
  bool lowSide = false;    // read every threshold the other way up
  // Applied instead of minCritical/confirmMs once any reading in the window was corrected.
  int minCriticalCorrected = 3;
  uint32_t confirmMsCorrected = 120'000;
};

inline bool alarmBeyond(const AlarmConfig& cfg, int hr, int threshold) {
  return cfg.lowSide ? hr <= threshold : hr >= threshold;
}

// Plain-old-data so the whole thing can live in RTC memory across a soft reboot.
struct AlarmMachine {
  AlarmState state = AlarmState::IDLE;
  AlarmCause cause = AlarmCause::NONE;
  uint32_t onsetEpoch = 0;
  uint32_t resolvedEpoch = 0;
  bool resolved = false;
  int peak = 0;   // most extreme reading of the episode: the lowest on the low side

  int lastHr = 0;      // last valid reading in any state, for collapse detection
  bool haveLast = false;

  uint32_t windowStartMs = 0;
  int windowReadings = 0;
  int windowCritCount = 0;
  int windowLastHr = 0;
  bool windowResolving = true;   // moving strictly back toward the safe band so far

  uint32_t dismissedMs = 0;
  bool everDismissed = false;

  bool windowCorrected = false;  // any reading in this window came from the beat interval
  bool lastCorrected = false;    // whether the most recent one did, for a window restart
};

struct AlarmEvent {
  bool alarmStarted = false;
  bool episodeResolved = false;
};

inline bool alarmSnoozed(const AlarmMachine& m, const AlarmConfig& cfg, uint32_t nowMs) {
  if (!m.everDismissed) return false;
  return static_cast<uint32_t>(nowMs - m.dismissedMs) < cfg.snoozeMs;
}

// A window is "corrected" if any reading in it was, and stays so to the end: mixing one beat-derived
// reading into a window of raw ones is exactly the case that needs the longer look.
inline uint32_t alarmWindowMs(const AlarmMachine& m, const AlarmConfig& cfg) {
  return m.windowCorrected ? cfg.confirmMsCorrected : cfg.confirmMs;
}

inline int alarmWindowMinCritical(const AlarmMachine& m, const AlarmConfig& cfg) {
  return m.windowCorrected ? cfg.minCriticalCorrected : cfg.minCritical;
}

inline void alarmResetWindow(AlarmMachine& m, uint32_t nowMs, int seedHr, bool seedCorrected) {
  m.windowStartMs = nowMs;
  m.windowReadings = 1;
  m.windowCritCount = 0;
  m.windowLastHr = seedHr;
  m.windowResolving = true;
  m.windowCorrected = seedCorrected;
  m.lastCorrected = seedCorrected;
}

inline void alarmAccumulate(AlarmMachine& m, const AlarmConfig& cfg, int hr, bool corrected) {
  // Resolving means moving strictly back toward the safe band. Holding level breaks it exactly as
  // moving the wrong way does.
  const bool notRecovering = cfg.lowSide ? hr <= m.windowLastHr : hr >= m.windowLastHr;
  if (m.windowReadings > 0 && notRecovering) m.windowResolving = false;
  ++m.windowReadings;
  if (alarmBeyond(cfg, hr, cfg.crit)) ++m.windowCritCount;
  m.windowLastHr = hr;
  if (corrected) m.windowCorrected = true;
  m.lastCorrected = corrected;
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
  m.windowCorrected = false;
  m.lastCorrected = false;
  m.resolved = false;
  m.resolvedEpoch = 0;
}

inline AlarmEvent alarmOnReading(AlarmMachine& m, const AlarmConfig& cfg, int hr,
                                 uint32_t nowMs, uint32_t nowEpoch, bool corrected = false) {
  AlarmEvent event{};
  // Zero is the absence of a reading. Dropped here, before it can arm anything, seed a window or
  // pass for a recovery: staleness, not this, is what escalates a band that has gone quiet.
  if (cfg.lowSide && hr <= 0) return event;

  const int previous = m.lastHr;
  const bool hadPrevious = m.haveLast;
  m.lastHr = hr;
  m.haveLast = true;

  if (m.state == AlarmState::ALARM) {
    if (cfg.lowSide ? (hr < m.peak) : (hr > m.peak)) m.peak = hr;
    if (hr > 0 && !alarmBeyond(cfg, hr, cfg.crit) && !m.resolved) {
      m.resolved = true;
      m.resolvedEpoch = nowEpoch;
      event.episodeResolved = true;
    }
    return event;
  }

  // Implausible collapse: high-side, raw readings only (see the header comment).
  if (!cfg.lowSide && !corrected && hadPrevious && previous >= cfg.collapseFrom &&
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
      alarmResetWindow(m, nowMs, hr, corrected);
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
  alarmAccumulate(m, cfg, hr, corrected);
  return event;
}

// Called every loop pass. Takes no epoch on purpose: both alarm entries here use the onset stamped
// when the rate first crossed, and passing the current time would only invite using it.
inline AlarmEvent alarmTick(AlarmMachine& m, const AlarmConfig& cfg, uint32_t nowMs, bool stale) {
  AlarmEvent event{};
  if (m.state != AlarmState::ARMED) return event;

  if (stale) {
    // The last thing this saw was a critical rate, and then nothing. Silence is not a drop, and
    // the window cannot be completed without data.
    if (!alarmSnoozed(m, cfg, nowMs)) {
      alarmEnter(m, AlarmCause::LOST_WHILE_CRITICAL, m.onsetEpoch, event);
    }
    return event;
  }

  if (static_cast<uint32_t>(nowMs - m.windowStartMs) < alarmWindowMs(m, cfg)) return event;

  // A trend cannot be established from one point, and that point was beyond crit, so a
  // single-reading window fails toward alarming.
  const bool notResolving = m.windowReadings < 2 || !m.windowResolving;
  const bool confirmed = alarmBeyond(cfg, m.windowLastHr, cfg.sustain) &&
                         (m.windowCritCount >= alarmWindowMinCritical(m, cfg) || notResolving);

  if (confirmed && !alarmSnoozed(m, cfg, nowMs)) {
    const AlarmCause cause = cfg.lowSide
                               ? AlarmCause::CONFIRMED_LOW
                               : (m.windowCorrected ? AlarmCause::CONFIRMED_HIGH_BEAT
                                                    : AlarmCause::CONFIRMED_HIGH);
    alarmEnter(m, cause, m.onsetEpoch, event);
    return event;
  }

  // Not confirmed: start a fresh window without touching onsetEpoch. Resetting to IDLE here would
  // let a rate oscillating either side of crit run indefinitely without ever alarming.
  const int carried = m.windowLastHr;
  alarmResetWindow(m, nowMs, carried, m.lastCorrected);
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
