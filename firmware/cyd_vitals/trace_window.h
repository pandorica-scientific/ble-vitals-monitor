#pragma once

#include <stdint.h>

// Windowing for the alarm's high-resolution heart-rate trace. Pure logic.

// A break longer than the staleness limit is a dropout, not a slow update.
constexpr uint32_t TRACE_GAP_MS = 30'000;

// The window starts at ten minutes and widens to keep the onset on screen. A strict rolling
// ten minutes would scroll the start of the episode off the left edge, and when it started
// is precisely what a clinician asks.
inline uint32_t traceWindowMinutes(uint32_t elapsedS) {
  if (elapsedS < 10 * 60) return 10;
  if (elapsedS < 30 * 60) return 30;
  return 60;
}

struct TraceGap {
  uint32_t startEpoch = 0;
  uint32_t endEpoch = 0;
};

// Compared in seconds, not milliseconds: scaling the difference up by 1000 would overflow
// uint32_t for stale entries and report a gap as no gap at all.
inline bool traceIsGap(uint32_t earlierEpoch, uint32_t laterEpoch) {
  if (laterEpoch <= earlierEpoch) return false;
  return (laterEpoch - earlierEpoch) > TRACE_GAP_MS / 1000u;
}

// History reloaded from CSV before the clock was set carries no usable timestamps. Saying so
// is better than placing those samples at epoch zero and drawing a confident wrong axis.
inline bool tracePositional(const uint32_t* epochs, int count) {
  if (epochs == nullptr) return true;
  for (int i = 0; i < count; ++i) {
    if (epochs[i] == 0) return true;
  }
  return false;
}
