#pragma once

#include <stdint.h>

// The most recent readings, for the one-hour sparklines and the alarm trace. Pure logic.
//
// Each sample carries its epoch second, or 0 when the clock was not set. The time axis is what lets
// a four-minute dropout be drawn as a gap instead of a straight line that reads as a steady rate.

// About an hour at one measurement every ~20 s.
constexpr int HISTORY_LEN = 180;

struct History {
  uint8_t heartRate[HISTORY_LEN] = {};
  uint8_t oxygen[HISTORY_LEN] = {};
  uint32_t epoch[HISTORY_LEN] = {};
  int head = 0;   // next slot to write
  int count = 0;  // filled slots, at most HISTORY_LEN
};

inline uint8_t historyClampByte(int v) {
  if (v < 0) return 0;
  if (v > 254) return 254;
  return static_cast<uint8_t>(v);
}

inline void historyPush(History& h, int heartRate, int oxygen, uint32_t epoch) {
  h.heartRate[h.head] = historyClampByte(heartRate);
  h.oxygen[h.head] = historyClampByte(oxygen);
  h.epoch[h.head] = epoch;
  h.head = (h.head + 1) % HISTORY_LEN;
  if (h.count < HISTORY_LEN) ++h.count;
}

// Slot of the i-th oldest sample (0 = oldest). Valid for 0 <= i < count.
inline int historyOldest(const History& h, int i) {
  return (h.head - h.count + i + HISTORY_LEN) % HISTORY_LEN;
}

// Slot of the i-th newest sample (0 = newest). Valid for 0 <= i < count.
inline int historyNewest(const History& h, int i) {
  return (h.head - 1 - i + 2 * HISTORY_LEN) % HISTORY_LEN;
}

// The latest epoch recorded, or 0 when no sample carries one.
inline uint32_t historyNewestEpoch(const History& h) {
  uint32_t newest = 0;
  for (int i = 0; i < h.count; ++i) {
    if (h.epoch[i] > newest) newest = h.epoch[i];
  }
  return newest;
}
