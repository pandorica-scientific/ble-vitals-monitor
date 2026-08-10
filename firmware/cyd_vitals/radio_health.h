#pragma once

#include <stdint.h>

constexpr uint32_t RADIO_START_GRACE_MS = 30'000;
constexpr uint32_t RADIO_TRAFFIC_STALE_MS = 30'000;
constexpr uint32_t RADIO_RESTART_COOLDOWN_MS = 60'000;

enum class RadioState : uint8_t { STARTING, RECEIVING, BAND_MISSING, SCANNER_SILENT };

struct RadioHealthInput {
  uint32_t nowMs = 0;
  bool scanStarted = false;
  uint32_t scanStartedMs = 0;
  bool anySeen = false;
  uint32_t lastAnyMs = 0;
  bool bandSeen = false;
  uint32_t lastBandMs = 0;
};

inline bool radioRecent(uint32_t nowMs, uint32_t eventMs, uint32_t limitMs) {
  return static_cast<uint32_t>(nowMs - eventMs) <= limitMs;
}

inline RadioState classifyRadioHealth(const RadioHealthInput& input) {
  if (!input.scanStarted) return RadioState::SCANNER_SILENT;
  if (input.bandSeen && radioRecent(input.nowMs, input.lastBandMs, RADIO_TRAFFIC_STALE_MS)) {
    return RadioState::RECEIVING;
  }
  if (static_cast<uint32_t>(input.nowMs - input.scanStartedMs) < RADIO_START_GRACE_MS) {
    return RadioState::STARTING;
  }
  if (input.anySeen && radioRecent(input.nowMs, input.lastAnyMs, RADIO_TRAFFIC_STALE_MS)) {
    return RadioState::BAND_MISSING;
  }
  return RadioState::SCANNER_SILENT;
}

inline bool shouldRestartScan(RadioState state, bool attempted, uint32_t nowMs, uint32_t lastAttemptMs) {
  if (state != RadioState::SCANNER_SILENT) return false;
  return !attempted || static_cast<uint32_t>(nowMs - lastAttemptMs) >= RADIO_RESTART_COOLDOWN_MS;
}
