#pragma once

#include <stdio.h>
#include <string.h>

#include "band_protocol.h"
#include "config.h"
#include "display.h"
#include "history.h"
#include "live_render_key.h"
#include "radio_health.h"

// The live view: header, the two big numbers, two one-hour sparklines and the alert bar.
//
// Each region clears its own rectangle. A full-screen wipe on every new reading (about every 20 s)
// reads as a blink, so the full wipe happens only on a genuine full redraw: a theme change, or a
// return from another view. drawLive() compares the new and previous LiveRenderKey to decide which
// regions need repainting.

inline void drawLiveCell(const Screen& s, int col, int row, const char* label, const char* value,
                         const char* unit, uint16_t colour) {
  auto& tft = s.tft;
  const Layout& L = s.layout;
  const int x = col * L.colW;
  const int y = L.headerH + row * L.rowH;
  tft.fillRect(x, y, L.colW, L.rowH, s.palette.bg);
  tft.setTextDatum(textdatum_t::top_left);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(s.palette.grey);
  tft.drawString(label, x + 8, y + 2);
  tft.setTextDatum(textdatum_t::bottom_right);
  tft.drawString(unit, x + L.colW - 8, y + L.rowH - 4);
  tft.setFont(&fonts::Font7);
  tft.setTextSize(1);
  tft.setTextColor(colour);
  tft.setTextDatum(textdatum_t::top_left);
  tft.drawString(value, x + 8, y + 20);
}

// Name, skin temperature, clock and the radio status. `clockText` is "HH:MM" or empty.
inline void drawLiveHeader(const Screen& s, const ReadingSnapshot& reading, RadioState radioState,
                           bool stale, const char* clockText) {
  auto& tft = s.tft;
  const Layout& L = s.layout;
  tft.fillRect(0, 2, L.w, L.headerH - 4, s.palette.bg);

  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextDatum(textdatum_t::top_left);
  tft.setTextColor(s.palette.grey);
  const int nameWidth = static_cast<int>(tft.drawString(DISPLAY_NAME, 6, 4));

  if (reading.skinValid && !stale) {
    char skin[10];
    snprintf(skin, sizeof(skin), "%.1fC", static_cast<double>(reading.skinC));
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextColor(COLOUR_SKIN);
    tft.drawString(skin, 6 + nameWidth + 14, 5);
  }

  const bool haveClock = clockText != nullptr && clockText[0] != '\0';
  char status[40];
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(textdatum_t::top_right);
  if (radioState == RadioState::RECEIVING) {
    tft.setTextColor(s.palette.grey);
    snprintf(status, sizeof(status), "%s%ssig%d", haveClock ? clockText : "",
             haveClock ? "  " : "", reading.signal);
  } else {
    const char* label = radioState == RadioState::STARTING ? "scan"
                        : radioState == RadioState::BAND_MISSING ? "BAND / RANGE"
                                                                  : "RADIO RETRY";
    tft.setTextColor(COLOUR_STATUS_WARN);
    snprintf(status, sizeof(status), "%s%s%s", haveClock ? clockText : "", haveClock ? "  " : "",
             label);
  }
  tft.drawString(status, L.w - 6, 5);
}

// One-hour sparkline with caution and danger reference lines.
inline void drawLiveSparkline(const Screen& s, const History& hist, int x, int y, int w, int h,
                              bool heart) {
  auto& tft = s.tft;
  const uint8_t* data = heart ? hist.heartRate : hist.oxygen;
  const float ymin = heart ? 50.0f : 80.0f;
  const float ymax = heart ? 250.0f : 100.0f;
  const float refCaution = heart ? 160.0f : 92.0f;
  const float refDanger = heart ? 200.0f : 90.0f;
  const uint16_t colour = heart ? COLOUR_HEART : COLOUR_OXYGEN;
  // Reserves a strip for the label, so a reading at the top of the scale does not overprint it.
  const int topPad = 11;

  tft.fillRect(x, y, w, h, s.palette.bg);
  tft.drawRect(x, y, w, h, s.palette.line);
  auto Y = [&](float v) {
    if (v < ymin) v = ymin;
    if (v > ymax) v = ymax;
    return y + h - 2 - static_cast<int>((v - ymin) / (ymax - ymin) * (h - 3 - topPad));
  };
  tft.drawFastHLine(x + 1, Y(refCaution), w - 2, PANEL_YELLOW);
  tft.drawFastHLine(x + 1, Y(refDanger), w - 2, PANEL_RED);
  tft.setFont(&fonts::Font0);
  tft.setTextColor(s.palette.grey);
  tft.setTextDatum(textdatum_t::top_left);
  tft.drawString(heart ? "BPM 1h" : "SpO2 1h", x + 3, y + 2);

  const int n = hist.count;
  if (n < 2) return;
  int px = -1;
  int py = -1;
  for (int i = 0; i < n; ++i) {
    const int v = data[historyOldest(hist, i)];
    const int xx = x + 1 + static_cast<int>(static_cast<long>(i) * (w - 3) / (n - 1));
    const int yy = Y(static_cast<float>(v));
    if (px >= 0) tft.drawLine(px, py, xx, yy, colour);
    px = xx;
    py = yy;
  }
}

inline void drawLiveAlertBar(const Screen& s, uint8_t alertMask) {
  auto& tft = s.tft;
  const Layout& L = s.layout;
  char text[48] = "ALERT:";
  if (alertMask & ALERT_HR_LOW) strlcat(text, " HR LOW", sizeof(text));
  if (alertMask & ALERT_HR_HIGH) strlcat(text, " HR HIGH", sizeof(text));
  if (alertMask & ALERT_SPO2_LOW) strlcat(text, " SpO2 LOW", sizeof(text));
  const int barY = L.h - 20;
  tft.fillRect(0, barY, L.w, 20, PANEL_YELLOW);
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextColor(PANEL_BLACK);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString(text, L.w / 2, barY + 10);
}

inline void drawLive(const Screen& s, const History& hist, const ReadingSnapshot& reading,
                     RadioState radioState, const char* clockText, const LiveRenderKey& key,
                     const LiveRenderKey& prev, bool full) {
  auto& tft = s.tft;
  const Layout& L = s.layout;
  const Palette& P = s.palette;
  const bool stale = key.stale;

  if (full) {
    tft.fillScreen(P.bg);
    tft.drawFastHLine(0, L.headerH - 2, L.w, P.line);
  }

  if (full || key.minuteKey != prev.minuteKey || key.signal != prev.signal ||
      key.radioState != prev.radioState || key.stale != prev.stale ||
      key.skinTenths != prev.skinTenths || key.skinValid != prev.skinValid) {
    drawLiveHeader(s, reading, radioState, stale, clockText);
  }

  char heartText[8];
  char oxygenText[8];
  const int shownHr = reading.effectiveHeartRate;
  if (stale || shownHr == 0) {
    strlcpy(heartText, "--", sizeof(heartText));
  } else {
    snprintf(heartText, sizeof(heartText), "%d", shownHr);
  }
  if (stale || reading.oxygenSaturation == 0) {
    strlcpy(oxygenText, "--", sizeof(oxygenText));
  } else {
    snprintf(oxygenText, sizeof(oxygenText), "%d", reading.oxygenSaturation);
  }

  if (full || key.effectiveHeartRate != prev.effectiveHeartRate ||
      key.heartRate != prev.heartRate || key.stale != prev.stale ||
      key.corrected != prev.corrected || key.readingIssue != prev.readingIssue ||
      key.intervalHeartRate != prev.intervalHeartRate) {
    const bool outOfBand = shownHr != 0 && (shownHr < HR_LOW || shownHr > HR_HIGH);
    const uint16_t colour = stale           ? P.dim
                            : key.corrected ? COLOUR_CORRECTED
                            : outOfBand     ? PANEL_YELLOW
                                            : COLOUR_HEART;
    drawLiveCell(s, 0, 0, "HEART", heartText, "bpm", colour);
    // Under the number: what the band's byte said against what its beat timing said. Either decode
    // can lock onto every second beat, so whichever is shown above, the other is worth seeing.
    // "reading issue" means the interval was too wild to state as a rate.
    if (key.corrected || key.readingIssue) {
      char note[32];
      if (key.intervalHeartRate > 0) {
        snprintf(note, sizeof(note), "band %d / beat %d", reading.heartRate,
                 key.intervalHeartRate);
      } else {
        strlcpy(note, "reading issue", sizeof(note));
      }
      tft.setFont(&fonts::Font0);
      tft.setTextSize(1);
      tft.setTextColor(stale ? P.dim : COLOUR_CORRECTED);
      tft.setTextDatum(textdatum_t::bottom_left);
      tft.drawString(note, 8, L.headerH + L.rowH - 4);
    }
  }

  if (full || key.oxygenSaturation != prev.oxygenSaturation || key.stale != prev.stale) {
    const bool low = reading.oxygenSaturation != 0 && reading.oxygenSaturation < SPO2_LOW;
    drawLiveCell(s, 1, 0, "OXYGEN", oxygenText, "%",
                 stale ? P.dim : (low ? PANEL_YELLOW : COLOUR_OXYGEN));
  }

  // The sparklines are also redrawn when the alert bar appears or clears, because the bar overlaps
  // their bottom rows and would leave a hole behind it otherwise.
  const int plotY = L.headerH + L.rowH;
  if (full || key.sequence != prev.sequence || key.alertMask != prev.alertMask) {
    drawLiveSparkline(s, hist, 2, plotY + 2, L.colW - 3, L.rowH - 4, true);
    drawLiveSparkline(s, hist, L.colW + 1, plotY + 2, L.colW - 3, L.rowH - 4, false);
  }
  if (key.alertMask != ALERT_NONE) drawLiveAlertBar(s, key.alertMask);
}
