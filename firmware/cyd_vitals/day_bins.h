#pragma once

#include <math.h>
#include <stdint.h>

// One day of readings in fifteen-minute bins, for the 24-hour touch chart. Pure logic.
//
// Only running sums are kept, so the chart can be redrawn at any bin width by grouping adjacent
// bins. A zero reading is "no reading" and is not added.

constexpr int DAY_BIN_MINUTES = 15;
constexpr int DAY_BIN_COUNT = 1440 / DAY_BIN_MINUTES;   // 96

enum class DayMetric : uint8_t { HEART_RATE, OXYGEN };

struct DayBin {
  uint16_t heartCount = 0;
  uint16_t oxygenCount = 0;
  float heartSum = 0.0f;
  float heartSumSq = 0.0f;
  float oxygenSum = 0.0f;
  float oxygenSumSq = 0.0f;
};

struct DayBins {
  DayBin bin[DAY_BIN_COUNT] = {};
};

struct BinStats {
  uint32_t count = 0;
  float mean = 0.0f;
  float sd = 0.0f;   // population standard deviation
};

inline void dayBinsAdd(DayBins& d, int heartRate, int oxygen, int minuteOfDay) {
  if (minuteOfDay < 0) return;
  const int b = minuteOfDay / DAY_BIN_MINUTES;
  if (b >= DAY_BIN_COUNT) return;
  DayBin& bin = d.bin[b];
  if (heartRate > 0) {
    ++bin.heartCount;
    bin.heartSum += static_cast<float>(heartRate);
    bin.heartSumSq += static_cast<float>(heartRate) * static_cast<float>(heartRate);
  }
  if (oxygen > 0) {
    ++bin.oxygenCount;
    bin.oxygenSum += static_cast<float>(oxygen);
    bin.oxygenSumSq += static_cast<float>(oxygen) * static_cast<float>(oxygen);
  }
}

// Mean and standard deviation over `groupCount` consecutive bins starting at `firstBin`.
inline BinStats dayBinsStats(const DayBins& d, DayMetric metric, int firstBin, int groupCount) {
  BinStats out;
  float sum = 0.0f;
  float sumSq = 0.0f;
  for (int i = firstBin; i < firstBin + groupCount && i < DAY_BIN_COUNT; ++i) {
    if (i < 0) continue;
    const DayBin& bin = d.bin[i];
    if (metric == DayMetric::HEART_RATE) {
      out.count += bin.heartCount;
      sum += bin.heartSum;
      sumSq += bin.heartSumSq;
    } else {
      out.count += bin.oxygenCount;
      sum += bin.oxygenSum;
      sumSq += bin.oxygenSumSq;
    }
  }
  if (out.count == 0) return out;
  out.mean = sum / static_cast<float>(out.count);
  float variance = sumSq / static_cast<float>(out.count) - out.mean * out.mean;
  if (variance < 0.0f) variance = 0.0f;   // rounding can push a constant series just below zero
  out.sd = sqrtf(variance);
  return out;
}
