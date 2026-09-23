#pragma once

#include <math.h>
#include <stdio.h>

#include "day_bins.h"
#include "display.h"

// The 24-hour chart: mean and standard deviation per bin, midnight to midnight. Opened by tapping
// a column of the live view; a tap on the chart cycles the bin width.

inline void drawDayPlot(const Screen& s, const DayBins& bins, DayMetric metric, int binMinutes) {
  auto& tft = s.tft;
  const Layout& L = s.layout;
  const Palette& P = s.palette;
  const bool oxygen = metric == DayMetric::OXYGEN;
  const int group = binMinutes / DAY_BIN_MINUTES;   // fifteen-minute bins per plotted bin
  const int plotted = DAY_BIN_COUNT / group;
  const float ymin = oxygen ? 70.0f : 40.0f;
  const float ymax = oxygen ? 100.0f : 200.0f;
  const uint16_t colour = oxygen ? COLOUR_OXYGEN : COLOUR_HEART;
  const int px0 = 34;
  const int py0 = 30;
  const int px1 = L.w - 6;
  const int py1 = L.h - 24;

  tft.fillScreen(P.bg);

  char title[32];
  snprintf(title, sizeof(title), "%s 24h (%dm)", oxygen ? "OXYGEN" : "HEART", binMinutes);
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextColor(P.grey);
  tft.setTextDatum(textdatum_t::top_center);
  tft.drawString(title, L.w / 2, 5);

  tft.drawRect(px0, py0, px1 - px0, py1 - py0, P.line);

  auto Y = [&](float v) {
    if (v < ymin) v = ymin;
    if (v > ymax) v = ymax;
    return py1 - static_cast<int>((v - ymin) / (ymax - ymin) * (py1 - py0));
  };

  // Horizontal grid with value labels.
  tft.setFont(&fonts::Font0);
  tft.setTextColor(P.dim);
  for (int k = 0; k <= 4; ++k) {
    const float v = ymin + (ymax - ymin) * k / 4;
    const int y = Y(v);
    tft.drawFastHLine(px0, y, px1 - px0, P.grid);
    char label[8];
    snprintf(label, sizeof(label), "%d", static_cast<int>(v));
    tft.setTextDatum(textdatum_t::middle_right);
    tft.drawString(label, px0 - 2, y);
  }
  // Vertical grid every six hours.
  for (int hour = 0; hour <= 24; hour += 6) {
    const int x = px0 + static_cast<int>(static_cast<float>(hour) / 24 * (px1 - px0));
    tft.drawFastVLine(x, py0, py1 - py0, P.grid);
    char label[4];
    snprintf(label, sizeof(label), "%d", hour);
    tft.setTextDatum(textdatum_t::top_center);
    tft.drawString(label, x, py1 + 2);
  }

  // One dot per bin at the mean, with a dim bar spanning one standard deviation either side.
  const float binWidth = static_cast<float>(px1 - px0) / plotted;
  for (int i = 0; i < plotted; ++i) {
    const BinStats st = dayBinsStats(bins, metric, i * group, group);
    if (st.count == 0) continue;
    const int cx = px0 + static_cast<int>((i + 0.5f) * binWidth);
    const int yLow = Y(st.mean - st.sd);
    const int yHigh = Y(st.mean + st.sd);
    tft.drawFastVLine(cx, yHigh, yLow - yHigh, dim565(colour));
    tft.fillCircle(cx, Y(st.mean), 2, colour);
  }

  tft.setTextColor(P.dim);
  tft.setTextDatum(textdatum_t::bottom_right);
  tft.setFont(&fonts::Font0);
  tft.drawString("tap: change bin  -  auto-back 10s", L.w - 4, L.h - 2);
}
