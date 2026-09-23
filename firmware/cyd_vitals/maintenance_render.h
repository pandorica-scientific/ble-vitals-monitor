#pragma once

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "display.h"

// The screen shown while the board sits on the home network waiting for a firmware upload. Same
// shape as the export screen: what to connect to, a warning that monitoring is paused, a countdown.

enum class MaintenancePhase : uint8_t { CONNECTING, NO_NETWORK, NO_PASSWORD, READY };

struct MaintenanceStatus {
  MaintenancePhase phase = MaintenancePhase::CONNECTING;
  char network[33] = {};   // an SSID is at most 32 bytes
  char address[16] = {};   // dotted IPv4
  const char* hostname = "";
};

constexpr int MAINTENANCE_ROW_Y0 = 34;
constexpr int MAINTENANCE_ROW_DY = 52;

inline void drawMaintenanceRow(const Screen& s, int y, const char* label, const char* value) {
  auto& tft = s.tft;
  const Layout& L = s.layout;
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(s.palette.dim);
  tft.setTextDatum(textdatum_t::top_center);
  tft.drawString(label, L.w / 2, y);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextColor(s.palette.grey);
  tft.drawString(value, L.w / 2, y + 15);
}

// Clears the area between the title rule and the warning bar.
inline void clearMaintenanceBody(const Screen& s) {
  const Layout& L = s.layout;
  s.tft.fillRect(0, 26, L.w, L.h - 42 - 26, s.palette.bg);
}

inline void drawMaintenanceStatic(const Screen& s, const MaintenanceStatus& st) {
  auto& tft = s.tft;
  const Layout& L = s.layout;
  const Palette& P = s.palette;
  tft.fillScreen(P.bg);
  tft.setTextDatum(textdatum_t::top_center);
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextColor(P.grey);
  tft.drawString("FIRMWARE UPDATE", L.w / 2, 5);
  tft.drawFastHLine(0, 24, L.w, P.line);

  const int y0 = MAINTENANCE_ROW_Y0;
  const int dy = MAINTENANCE_ROW_DY;
  switch (st.phase) {
    case MaintenancePhase::CONNECTING:
      drawMaintenanceRow(s, y0, "Joining home network", "please wait");
      break;
    case MaintenancePhase::NO_NETWORK:
      drawMaintenanceRow(s, y0, "No network reachable", "resuming monitoring");
      break;
    case MaintenancePhase::NO_PASSWORD:
      drawMaintenanceRow(s, y0, "Network", st.network);
      drawMaintenanceRow(s, y0 + dy, "Address", st.address);
      drawMaintenanceRow(s, y0 + 2 * dy, "No /ota.txt on the card", "upload disabled");
      break;
    case MaintenancePhase::READY: {
      char host[48];
      snprintf(host, sizeof(host), "%s.local", st.hostname);
      drawMaintenanceRow(s, y0, "Network", st.network);
      drawMaintenanceRow(s, y0 + dy, "Address", st.address);
      drawMaintenanceRow(s, y0 + 2 * dy, "Hostname", host);
      break;
    }
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
inline void drawMaintenanceCountdown(const Screen& s, uint32_t secondsLeft) {
  auto& tft = s.tft;
  const Layout& L = s.layout;
  char text[40];
  snprintf(text, sizeof(text), "auto-resume in %lu:%02lu",
           static_cast<unsigned long>(secondsLeft / 60),
           static_cast<unsigned long>(secondsLeft % 60));
  tft.fillRect(0, L.h - 16, L.w, 16, s.palette.bg);
  tft.setFont(&fonts::Font0);
  tft.setTextColor(s.palette.dim);
  tft.setTextDatum(textdatum_t::top_center);
  tft.drawString(text, L.w / 2, L.h - 13);
}

// Upload progress, drawn from the OTA callbacks while the transfer blocks the loop.
inline void drawMaintenanceProgress(const Screen& s, unsigned done, unsigned total) {
  auto& tft = s.tft;
  const Layout& L = s.layout;
  const Palette& P = s.palette;
  const unsigned percent = total ? static_cast<unsigned>((static_cast<uint64_t>(done) * 100u) / total) : 0;
  static unsigned lastPercent = 101;
  if (percent == lastPercent) return;   // the callback fires per chunk; the bar only per percent
  if (percent < lastPercent) clearMaintenanceBody(s);
  lastPercent = percent;

  const int barX = 20;
  const int barW = L.w - 40;
  const int barY = L.h / 2 - 8;
  const int barH = 16;
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(P.dim);
  tft.setTextDatum(textdatum_t::bottom_center);
  tft.drawString("Receiving firmware - do not unplug", L.w / 2, barY - 10);
  tft.drawRect(barX, barY, barW, barH, P.line);
  tft.fillRect(barX + 2, barY + 2, static_cast<int>((barW - 4) * percent / 100), barH - 4, P.grey);
  char text[8];
  snprintf(text, sizeof(text), "%u%%", percent);
  tft.fillRect(L.w / 2 - 30, barY + barH + 6, 60, 22, P.bg);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextColor(P.grey);
  tft.setTextDatum(textdatum_t::top_center);
  tft.drawString(text, L.w / 2, barY + barH + 6);
}

// A two-line notice in place of the rows: "Update installed" or the reason it failed.
inline void drawMaintenanceMessage(const Screen& s, const char* line1, const char* line2) {
  clearMaintenanceBody(s);
  drawMaintenanceRow(s, MAINTENANCE_ROW_Y0 + MAINTENANCE_ROW_DY, line1, line2);
}
