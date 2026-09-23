#pragma once

#include <stdio.h>

#include "config.h"
#include "display.h"

// The screen shown while the board is an access point serving the logs. It has to be readable at
// night too, so the caller forces full brightness before drawing it.

inline void drawExportStatic(const Screen& s) {
  auto& tft = s.tft;
  const Layout& L = s.layout;
  const Palette& P = s.palette;
  tft.fillScreen(P.bg);
  tft.setTextDatum(textdatum_t::top_center);
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextColor(P.grey);
  tft.drawString("DATA EXPORT", L.w / 2, 5);
  tft.drawFastHLine(0, 24, L.w, P.line);

  struct Row {
    const char* label;
    const char* value;
  };
  const Row rows[] = {
      {"Wi-Fi network", AP_SSID},
      {"Password", AP_PASS},
      {"Open in browser", "http://192.168.4.1"},
  };
  int y = 34;
  for (const Row& r : rows) {
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextColor(P.dim);
    tft.setTextDatum(textdatum_t::top_center);
    tft.drawString(r.label, L.w / 2, y);
    tft.setFont(&fonts::FreeSansBold12pt7b);
    tft.setTextColor(P.grey);
    tft.drawString(r.value, L.w / 2, y + 15);
    y += 52;
  }

  // Monitoring really is off while this is up, in the same style as a vitals alert.
  const int barY = L.h - 42;
  tft.fillRect(0, barY, L.w, 20, PANEL_YELLOW);
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextColor(PANEL_BLACK);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString("MONITORING PAUSED", L.w / 2, barY + 10);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(P.dim);
  tft.setTextDatum(textdatum_t::top_center);
  tft.drawString("swipe down to resume", L.w / 2, barY + 24);
}

// Only the countdown strip changes, so it is redrawn on its own once a second.
inline void drawExportCountdown(const Screen& s, uint32_t secondsLeft, int clients) {
  auto& tft = s.tft;
  const Layout& L = s.layout;
  char text[56];
  snprintf(text, sizeof(text), "auto-resume in %lu:%02lu  (%d client%s)",
           static_cast<unsigned long>(secondsLeft / 60),
           static_cast<unsigned long>(secondsLeft % 60), clients, clients == 1 ? "" : "s");
  tft.fillRect(0, L.h - 16, L.w, 16, s.palette.bg);
  tft.setFont(&fonts::Font0);
  tft.setTextColor(s.palette.dim);
  tft.setTextDatum(textdatum_t::top_center);
  tft.drawString(text, L.w / 2, L.h - 13);
}
