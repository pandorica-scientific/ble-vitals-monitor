#pragma once

#include <stdint.h>

// Tap and swipe recognition from raw touch samples. Pure logic.
//
// Gestures resolve on lift-off, not on contact: a swipe begins as a touch, so acting on the first
// sample would fire the tap action at the start of every swipe. Press-and-hold is in hold_gesture.h
// because it has to resolve while the finger is still down.

enum class Gesture : uint8_t { NONE, TAP, SWIPE_UP, SWIPE_DOWN };

struct GestureTracker {
  bool touching = false;
  int startX = 0;
  int startY = 0;
  int lastX = 0;
  int lastY = 0;
  uint32_t startMs = 0;
};

struct GestureResult {
  Gesture gesture = Gesture::NONE;
  int tapX = 0;   // where a TAP began
  int tapY = 0;
};

// Feed one touch sample per loop pass. A gesture is reported on the first sample without contact.
inline GestureResult gestureUpdate(GestureTracker& g, bool touching, int x, int y, uint32_t nowMs,
                                   int swipeMinDy, int tapMaxMove, uint32_t tapMaxMs) {
  GestureResult out{};
  if (touching) {
    if (!g.touching) {
      g.touching = true;
      g.startX = g.lastX = x;
      g.startY = g.lastY = y;
      g.startMs = nowMs;
    } else {
      g.lastX = x;
      g.lastY = y;
    }
    return out;
  }
  if (!g.touching) return out;
  g.touching = false;

  const int dx = g.lastX - g.startX;
  const int dy = g.lastY - g.startY;
  const int adx = dx < 0 ? -dx : dx;
  const int ady = dy < 0 ? -dy : dy;
  if (ady >= swipeMinDy && ady > adx) {
    out.gesture = dy < 0 ? Gesture::SWIPE_UP : Gesture::SWIPE_DOWN;
    return out;
  }
  const uint32_t heldMs = static_cast<uint32_t>(nowMs - g.startMs);
  if (adx <= tapMaxMove && ady <= tapMaxMove && heldMs < tapMaxMs) {
    out.gesture = Gesture::TAP;
    out.tapX = g.startX;
    out.tapY = g.startY;
  }
  return out;   // otherwise: too short for a swipe, too long or too smeared for a tap
}
