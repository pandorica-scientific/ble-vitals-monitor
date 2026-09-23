#pragma once

#include <stdio.h>

#include "config.h"
#include "contacts.h"
#include "critical_alarm.h"
#include "display.h"
#include "history.h"
#include "hold_gesture.h"
#include "radio_health.h"
#include "trace_window.h"

// The alarm screen.
//
// Drawing is split so the flash does not repaint the whole panel; a full redraw at the flash rate
// flickers and makes the numbers hard to read, which defeats the point of putting emergency numbers
// on the screen at all.
//   drawAlarmStatic - everything that changes only when a value changes
//   drawAlarmTimer  - the elapsed time, once a second, in its own corner
//   drawAlarmFrame  - the perimeter, twice a second
//   drawAlarmHold   - the countdown band, while a finger is down

constexpr uint16_t ALARM_BACKGROUND = PANEL_WHITE;
constexpr uint16_t ALARM_TEXT = PANEL_BLACK;
constexpr uint16_t ALARM_RED = PANEL_RED;
constexpr uint16_t ALARM_FRAME_OFF = 0x4000;   // the frame's off phase and the reference line
constexpr uint16_t ALARM_GAP = 0x39E7;         // shaded band over a dropout in the trace

constexpr int ALARM_FRAME_PX = 6;
// The trace gives up some height so the telephone numbers can be set in a real font. At three in
// the morning the numbers have to be readable at arm's length; the trace only has to show a shape.
constexpr int ALARM_TRACE_Y = 78;
constexpr int ALARM_TRACE_H = 70;
constexpr int ALARM_HOLD_Y = 160;
constexpr int ALARM_HOLD_H = 24;
constexpr int ALARM_CONTACT_Y = 188;
constexpr int ALARM_CONTACT_DY = 20;
constexpr int ALARM_TIMER_W = 66;   // room for "59:59" at FreeSansBold9pt7b, and no more
constexpr int ALARM_TRACE_HR_MIN = 60;    // fixed scale, so a rising rate cannot look flat
constexpr int ALARM_TRACE_HR_MAX = 260;

struct AlarmAppearance {
  const AlarmMachine* machine = nullptr;
  const Contacts* contacts = nullptr;
  int heartRate = 0;      // the rate on screen: corrected where the band needed correcting
  int rawHeartRate = 0;   // the band's own byte, shown small when the two differ
  bool corrected = false;
  int oxygen = 0;
  int oxygenAgeMin = -1;  // -1 when unknown
  uint32_t elapsedS = 0;
  bool selfTest = false;
  int selfTestLeftS = 0;
  bool stale = false;     // no packet within STALE_MS
  RadioState radioState = RadioState::RECEIVING;
};

// Only the perimeter flashes: a flashing background makes the numbers and the trace unreadable, and
// a frame is more visible from across a room than a header band.
inline void drawAlarmFrame(const Screen& s, bool on) {
  auto& tft = s.tft;
  const Layout& L = s.layout;
  const int t = ALARM_FRAME_PX;
  const uint16_t c = on ? ALARM_RED : ALARM_FRAME_OFF;
  tft.fillRect(0, 0, L.w, t, c);
  tft.fillRect(0, L.h - t, L.w, t, c);
  tft.fillRect(0, 0, t, L.h, c);
  tft.fillRect(L.w - t, 0, t, L.h, c);
}

// `refHr` is the threshold this episode is about (200 for a fast alarm, 80 for a slow one), so the
// dashed line is the one that matters.
inline void drawAlarmTrace(const Screen& s, const History& hist, int x, int y, int w, int h,
                           uint32_t elapsedS, int refHr) {
  auto& tft = s.tft;
  const int lo = ALARM_TRACE_HR_MIN;
  const int hi = ALARM_TRACE_HR_MAX;
  tft.fillRect(x, y, w, h, ALARM_BACKGROUND);
  const uint32_t windowMin = traceWindowMinutes(elapsedS);
  const uint32_t spanS = windowMin * 60;

  const int refY = y + h - ((refHr - lo) * h) / (hi - lo);
  for (int px = x; px < x + w; px += 6) tft.drawFastHLine(px, refY, 3, ALARM_FRAME_OFF);
  char ref[8];
  snprintf(ref, sizeof(ref), "%d", refHr);
  tft.setFont(&fonts::Font0);
  tft.setTextColor(ALARM_FRAME_OFF);
  tft.setTextDatum(textdatum_t::bottom_right);
  tft.drawString(ref, x + w - 2, refY - 1);

  tft.setTextColor(s.palette.dim);
  tft.setTextDatum(textdatum_t::top_right);
  tft.drawString("now", x + w, y + h + 2);

  if (hist.count < 2) {
    tft.setTextDatum(textdatum_t::top_left);
    tft.drawString("no history yet", x, y + h + 2);
    return;
  }

  // History reloaded before the clock was set has no usable timestamps; the trace then falls back
  // to a positional axis and says so, rather than drawing a confident wrong time axis.
  const uint32_t newest = historyNewestEpoch(hist);
  const bool timed = !tracePositional(hist.epoch, hist.count) && newest != 0;
  char axis[32];
  if (timed) {
    snprintf(axis, sizeof(axis), "last %u min", static_cast<unsigned>(windowMin));
  } else {
    snprintf(axis, sizeof(axis), "last %d readings", hist.count);
  }
  tft.setTextDatum(textdatum_t::top_left);
  tft.drawString(axis, x, y + h + 2);

  int prevX = -1;
  int prevY = -1;
  uint32_t prevEpoch = 0;
  for (int i = hist.count - 1; i >= 0; --i) {   // oldest to newest
    const int k = historyNewest(hist, i);
    const int hr = hist.heartRate[k];
    const uint32_t e = hist.epoch[k];
    if (hr <= 0) {
      prevX = -1;
      continue;
    }
    int px;
    if (!timed) {
      px = x + (w * (hist.count - 1 - i)) / (hist.count - 1);
    } else {
      if (newest - e > spanS) {
        prevX = -1;
        prevEpoch = e;
        continue;
      }
      px = x + w - static_cast<int>((newest - e) * static_cast<uint32_t>(w) / spanS);
    }
    int v = hr;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    const int py = y + h - ((v - lo) * h) / (hi - lo);
    if (prevX >= 0) {
      // A dropout is a shaded band, not a line and not blank space: a line across a four-minute
      // gap reads as a steady rate, blank space reads as "not filled up yet".
      if (timed && prevEpoch != 0 && e != 0 && traceIsGap(prevEpoch, e)) {
        tft.fillRect(prevX, y, px - prevX, h, ALARM_GAP);
      } else {
        tft.drawLine(prevX, prevY, px, py, ALARM_RED);
      }
    }
    prevX = px;
    prevY = py;
    prevEpoch = e;
  }
}

inline void drawAlarmStatic(const Screen& s, const History& hist, const AlarmAppearance& a) {
  auto& tft = s.tft;
  const Layout& L = s.layout;
  const int t = ALARM_FRAME_PX;
  tft.fillRect(t, t, L.w - 2 * t, L.h - 2 * t, ALARM_BACKGROUND);

  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextColor(ALARM_TEXT);
  tft.setTextDatum(textdatum_t::top_left);
  const bool low = a.machine && a.machine->cause == AlarmCause::CONFIRMED_LOW;
  // The beat-interval title tells whoever reads the screen that the number came from the band's
  // beat timing rather than its heart-rate byte, which is the first thing a clinician will ask
  // about a rate this receiver was not supposed to see.
  const bool beat = a.machine && a.machine->cause == AlarmCause::CONFIRMED_HIGH_BEAT;
  const char* title = low ? "LOW HEART RATE" : (beat ? "HIGH RATE (beat)" : "HIGH HEART RATE");
  tft.drawString(a.selfTest ? "TEST - SELF-CHECK" : title, 10, 10);

  // The byte cannot express more than 255, so at the rail the true rate is unknown and at least
  // this high: ">=255" is honest, 255 is not. The rail belongs to the byte; a corrected rate is
  // computed from an interval and can state a number above 255 exactly. A stale reading is "--",
  // because a number frozen ten minutes ago is not a measurement.
  char hr[12];
  if (a.stale) {
    snprintf(hr, sizeof(hr), "--");
  } else if (!a.corrected && a.heartRate >= HR_RAIL) {
    snprintf(hr, sizeof(hr), ">=255");
  } else if (a.heartRate <= 0) {
    snprintf(hr, sizeof(hr), "--");
  } else {
    snprintf(hr, sizeof(hr), "%d", a.heartRate);
  }
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextColor(ALARM_RED);
  tft.setTextDatum(textdatum_t::top_left);
  tft.drawString(hr, 10, 34);

  // The unit label doubles as the radio indicator. Without it a wristband dropout, which happens
  // whenever the baby moves, looks identical to a frozen screen.
  const char* unit = "bpm";
  bool warn = false;
  switch (a.radioState) {
    case RadioState::RECEIVING:
      unit = a.stale ? "NO DATA" : "bpm";
      warn = a.stale;
      break;
    case RadioState::STARTING:
      unit = "scan";
      warn = true;
      break;
    case RadioState::BAND_MISSING:
      unit = "NO BAND";
      warn = true;
      break;
    case RadioState::SCANNER_SILENT:
      unit = "RADIO RETRY";
      warn = true;
      break;
  }
  tft.setFont(&fonts::Font0);
  tft.setTextColor(warn ? ALARM_RED : s.palette.dim);
  tft.setTextDatum(textdatum_t::top_left);
  tft.drawString(unit, 10, 62);
  if (!warn) {
    // On the low side the extreme of the episode is its slowest reading.
    const int extreme = a.machine ? a.machine->peak : 0;
    char peak[36];
    if (a.corrected) {
      snprintf(peak, sizeof(peak), "%s %d - band %d", low ? "low" : "peak", extreme,
               a.rawHeartRate);
    } else {
      snprintf(peak, sizeof(peak), "%s %d", low ? "low" : "peak", extreme);
    }
    tft.setTextColor(s.palette.dim);
    tft.drawString(peak, 46, 62);
  }

  // Oxygen commits about every fifteen minutes, so the value beside a live heart rate may be a
  // quarter of an hour old. Its age is shown so nobody reads a stale number down the telephone.
  char oxygen[40];
  if (a.stale || a.oxygen <= 0) {
    snprintf(oxygen, sizeof(oxygen), "SpO2 --");
  } else if (a.oxygenAgeMin < 0) {
    snprintf(oxygen, sizeof(oxygen), "SpO2 %d%%", a.oxygen);
  } else {
    snprintf(oxygen, sizeof(oxygen), "SpO2 %d%% - %dm ago", a.oxygen, a.oxygenAgeMin);
  }
  tft.setTextColor(a.oxygenAgeMin >= SPO2_STALE_MIN ? s.palette.dim : ALARM_TEXT);
  tft.setTextDatum(textdatum_t::top_right);
  tft.drawString(oxygen, L.w - 10, 62);

  drawAlarmTrace(s, hist, 10, ALARM_TRACE_Y, L.w - 20, ALARM_TRACE_H, a.elapsedS,
                 low ? HR_CRIT_LOW : HR_CRIT);

  if (a.contacts && a.contacts->count > 0) {
    tft.setFont(&fonts::FreeSansBold9pt7b);
    tft.setTextColor(ALARM_TEXT);
    tft.setTextDatum(textdatum_t::top_center);
    for (int i = 0; i < a.contacts->count; ++i) {
      tft.drawString(a.contacts->line[i], L.w / 2, ALARM_CONTACT_Y + i * ALARM_CONTACT_DY);
    }
  }
}

// Repainted on its own once a second, so the tick does not drag the trace and the phone numbers
// through a full redraw.
inline void drawAlarmTimer(const Screen& s, const AlarmAppearance& a) {
  auto& tft = s.tft;
  const Layout& L = s.layout;
  char text[16];
  if (a.selfTest) {
    snprintf(text, sizeof(text), "%ds", a.selfTestLeftS);
  } else {
    snprintf(text, sizeof(text), "%02u:%02u", static_cast<unsigned>(a.elapsedS / 60),
             static_cast<unsigned>(a.elapsedS % 60));
  }
  // Only as wide as "59:59" needs: clearing from the middle would wipe the tail off the title.
  tft.fillRect(L.w - ALARM_TIMER_W - ALARM_FRAME_PX, ALARM_FRAME_PX, ALARM_TIMER_W, 26,
               ALARM_BACKGROUND);
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextColor(ALARM_TEXT);
  tft.setTextDatum(textdatum_t::top_right);
  tft.drawString(text, L.w - 10, 10);
}

// The countdown makes accidental dismissal essentially impossible (three seconds of deliberate
// contact is not something a sleeve produces) and makes it obvious the board has not frozen.
inline void drawAlarmHold(const Screen& s, const HoldResult& hold, bool selfTest) {
  auto& tft = s.tft;
  const Layout& L = s.layout;
  const int t = ALARM_FRAME_PX;
  tft.fillRect(t, ALARM_HOLD_Y, L.w - 2 * t, ALARM_HOLD_H, ALARM_BACKGROUND);
  tft.setTextDatum(textdatum_t::top_center);
  if (hold.active) {
    tft.setFont(&fonts::Font0);
    tft.setTextColor(s.palette.dim);
    tft.drawString(selfTest ? "HOLD TO TEST ALARM" : "HOLD TO DISMISS", L.w / 2, ALARM_HOLD_Y);
    char seconds[8];
    snprintf(seconds, sizeof(seconds), "%u", static_cast<unsigned>(hold.secondsLeft));
    tft.setFont(&fonts::FreeSansBold9pt7b);
    tft.setTextColor(ALARM_TEXT);
    tft.drawString(seconds, L.w / 2, ALARM_HOLD_Y + 10);
    const int barW = (L.w - 2 * t - 20) * hold.percent / 100;
    tft.fillRect(t + 10, ALARM_HOLD_Y + ALARM_HOLD_H - 3, barW, 3, ALARM_RED);
  } else if (!selfTest) {
    tft.setFont(&fonts::Font0);
    tft.setTextColor(s.palette.dim);
    tft.drawString("HOLD 3s TO DISMISS", L.w / 2, ALARM_HOLD_Y + 8);
  }
}
