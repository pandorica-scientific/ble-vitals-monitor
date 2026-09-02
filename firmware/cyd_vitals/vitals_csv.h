#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The daily vitals log. Pure formatting and parsing so both directions can be tested natively -
// the parser matters as much as the writer, because the board reloads the day's file at boot to
// refill its charts, and a column read one place to the left is a silently wrong graph.
//
// hr_bpm stays the band's raw byte and hr_eff carries the rate everything actually uses (see
// effectiveHeartRate() in band_protocol.h). Keeping both is the point: the raw column is what the
// report page already reads and what a later analysis needs to check the correction against, and
// beat_ms is the evidence for why a reading was corrected at all.
constexpr char VITALS_CSV_HEADER[] = "timestamp,hr_bpm,spo2_pct,skin_c,beat_ms,hr_eff";

struct VitalsRow {
  int hh = 0, mm = 0, ss = 0;
  int hr = 0;
  int spo2 = 0;
  float skinC = 0.0f;
  bool skinValid = false;
  int beatMs = 0;
  int hrEff = 0;
  bool haveEff = false;   // false for a row written before the columns existed
};

inline void formatVitalsRow(char* out, size_t cap, const char* timestamp, int hr, int spo2,
                            float skinC, bool skinValid, int beatMs, int hrEff) {
  if (out == nullptr || cap == 0) return;
  if (skinValid) {
    snprintf(out, cap, "%s,%d,%d,%.1f,%d,%d", timestamp == nullptr ? "" : timestamp,
             hr, spo2, static_cast<double>(skinC), beatMs, hrEff);
  } else {
    snprintf(out, cap, "%s,%d,%d,,%d,%d", timestamp == nullptr ? "" : timestamp,
             hr, spo2, beatMs, hrEff);
  }
}

// Accepts both the four-column rows written before this change and the six-column rows written
// after it, including the mixed file that upgrade day leaves behind: the header is only written
// when a file is created, so the day in progress keeps its old header and gains wider rows.
inline bool parseVitalsRow(const char* line, VitalsRow& out) {
  if (line == nullptr) return false;
  // "YYYY-MM-DD HH:MM:SS," - the timestamp is fixed width, so the clock is read by offset.
  if (strlen(line) < 20) return false;
  if (line[13] != ':' || line[16] != ':' || line[19] != ',') return false;
  out.hh = static_cast<int>(strtol(line + 11, nullptr, 10));
  out.mm = static_cast<int>(strtol(line + 14, nullptr, 10));
  out.ss = static_cast<int>(strtol(line + 17, nullptr, 10));
  if (out.hh < 0 || out.hh > 23 || out.mm < 0 || out.mm > 59 || out.ss < 0 || out.ss > 59) {
    return false;
  }

  const char* p = line + 20;
  out.hr = static_cast<int>(strtol(p, nullptr, 10));
  const char* c = strchr(p, ',');
  if (c == nullptr) return false;
  p = c + 1;
  out.spo2 = static_cast<int>(strtol(p, nullptr, 10));

  out.skinC = 0.0f;
  out.skinValid = false;
  out.beatMs = 0;
  out.hrEff = out.hr;
  out.haveEff = false;

  c = strchr(p, ',');
  if (c == nullptr) return true;           // three columns: no skin, no beat, no effective rate
  p = c + 1;
  if (*p != ',' && *p != '\0' && *p != '\r' && *p != '\n') {
    out.skinC = strtof(p, nullptr);
    out.skinValid = true;
  }

  c = strchr(p, ',');
  if (c == nullptr) return true;           // the old four-column row ends here
  p = c + 1;
  out.beatMs = static_cast<int>(strtol(p, nullptr, 10));

  c = strchr(p, ',');
  if (c == nullptr) return true;
  p = c + 1;
  if (*p == '\0' || *p == '\r' || *p == '\n') return true;
  out.hrEff = static_cast<int>(strtol(p, nullptr, 10));
  out.haveEff = true;
  return true;
}

// What the charts should plot for this row: the corrected rate where the file has one, and the
// raw byte for every row written before the correction existed.
inline int vitalsRowHistoryHr(const VitalsRow& row) {
  return row.haveEff ? row.hrEff : row.hr;
}
