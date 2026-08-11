#pragma once

#include <stdint.h>

// Press-and-hold with a visible countdown. Pure logic driven by injected touch state and time.
//
// Unlike readGesture()'s tap, this resolves WHILE still touching rather than on lift-off,
// because the countdown has to be drawn during the hold. Once it completes it must suppress
// the tap that would otherwise fire on release, or dismissing the alarm would also open a chart.

struct HoldState {
  bool holding = false;
  bool abandoned = false;
  bool fired = false;
  bool consumedTap = false;
  uint32_t startMs = 0;
  int startX = 0;
  int startY = 0;
};

struct HoldResult {
  bool active = false;
  uint8_t secondsLeft = 0;
  uint8_t percent = 0;
  bool completed = false;
};

inline void holdReset(HoldState& h) {
  h.holding = false;
  h.abandoned = false;
  h.fired = false;
  h.consumedTap = false;
  h.startMs = 0;
}

inline bool holdConsumedTap(const HoldState& h) { return h.consumedTap; }

inline HoldResult holdUpdate(HoldState& h, bool touching, int x, int y, uint32_t nowMs,
                             uint32_t holdMs, int maxMove) {
  HoldResult out{};

  if (!touching) {
    h.holding = false;
    h.abandoned = false;
    h.fired = false;
    h.consumedTap = false;
    return out;
  }

  if (!h.holding) {
    h.holding = true;
    h.abandoned = false;
    h.fired = false;
    h.startMs = nowMs;
    h.startX = x;
    h.startY = y;
  }

  const int dx = x - h.startX;
  const int dy = y - h.startY;
  const int adx = dx < 0 ? -dx : dx;
  const int ady = dy < 0 ? -dy : dy;
  if (adx > maxMove || ady > maxMove) h.abandoned = true;
  if (h.abandoned) return out;

  const uint32_t held = static_cast<uint32_t>(nowMs - h.startMs);
  out.active = true;

  if (held >= holdMs) {
    out.percent = 100;
    out.secondsLeft = 0;
    if (!h.fired) {
      h.fired = true;
      h.consumedTap = true;
      out.completed = true;
    }
    return out;
  }

  out.percent = static_cast<uint8_t>(held * 100u / holdMs);
  const uint32_t remainingMs = holdMs - held;
  out.secondsLeft = static_cast<uint8_t>((remainingMs + 999u) / 1000u);
  return out;
}
