#pragma once

// Day/night backlight scheduling driven by the sun rather than by two fixed clock times.
//
// Why solar at all: a fixed 20:00-08:00 window is wrong for half the year. In April the room is
// still bright at 20:00; in November it has been dark for hours. Sunset tracks the thing the
// dimming actually cares about - whether the room is lit - without anyone editing constants twice
// a year.
//
// Why the clamps: pure sunset/sunrise is worse than the fixed window at the solstices. At this
// latitude the sun rises at 04:14 in late June, and a monitor that jumps to full brightness at
// 04:14 in a sleeping household is a defect, not a feature. December is the mirror image: sunset
// at 15:25 would dim the screen mid-afternoon. So the solar times are clamped into a band that
// stays plausible as "the household is asleep". The result tracks the seasons through spring and
// autumn - the part a fixed window genuinely gets wrong - and refuses the extremes.
//
// No network is involved. The NOAA solar equation below needs only the date, a fixed location and
// the UTC offset already in effect from the configured timezone, so this keeps working with the
// radio busy and the WiFi down.

#include <math.h>
#include <stdint.h>
#include <time.h>

// Site location. Sunrise/sunset shift by about four minutes per degree of longitude, so this only
// needs to be right to within a degree or so to be right to within a few minutes.
constexpr double SITE_LATITUDE_DEG = 52.23;    // Warsaw
constexpr double SITE_LONGITUDE_DEG = 21.01;   // positive east

// Clamps on the solar result, in minutes since local midnight.
constexpr int NIGHT_START_EARLIEST_MIN = 19 * 60;   // never dim before 19:00, however early sunset is
constexpr int NIGHT_START_LATEST_MIN = 21 * 60;     // always dim by 21:00, however late sunset is
constexpr int NIGHT_END_EARLIEST_MIN = 7 * 60;      // never brighten before 07:00, however early sunrise is

// Backlight PWM levels. Current draw tracks duty closely, so these are close to their percentages.
constexpr int BRIGHT_DAY_LEVEL = 255;    // 100%
constexpr int BRIGHT_NIGHT_LEVEL = 26;   // ~10%

// Both transitions are ramped rather than stepped, starting at the boundary: the screen begins to
// fade at the night edge and reaches its night level an hour later, and mirrors that at dawn. The
// render loop refreshes the backlight every 30 s, so an hour spans ~120 steps of ~2 PWM levels -
// below the threshold where a change reads as a flicker.
constexpr int BRIGHT_RAMP_MIN = 60;

struct SolarTimes {
  int sunriseMin = -1;   // minutes since local midnight
  int sunsetMin = -1;
  bool valid = false;    // false inside the polar day/night, where neither event happens
};

struct NightWindow {
  int startMin = NIGHT_START_LATEST_MIN;
  int endMin = NIGHT_END_EARLIEST_MIN;
};

// Forward distance from `from` to `to` on a 24 h circle, so windows spanning midnight need no
// special casing anywhere else in this file.
inline int minutesForward(int from, int to) {
  int d = (to - from) % 1440;
  return d < 0 ? d + 1440 : d;
}

inline int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The UTC offset in effect, recovered by comparing local and UTC breakdowns of the same instant.
// The ESP32's newlib struct tm carries no tm_gmtoff, and doing it this way keeps the daylight
// saving rules in the one place that owns them - the configured timezone - rather than repeating
// them here where they could drift out of agreement.
inline int utcOffsetMinutes(const struct tm& local, const struct tm& utc) {
  int dayShift = local.tm_yday - utc.tm_yday;
  if (local.tm_year != utc.tm_year) {
    dayShift = local.tm_year > utc.tm_year ? 1 : -1;   // the two sides fell either side of New Year
  }
  return (dayShift * 24 + (local.tm_hour - utc.tm_hour)) * 60 + (local.tm_min - utc.tm_min);
}

// Julian date at 00:00 UT for a calendar date given as year plus zero-based day of year, which is
// exactly what struct tm carries in tm_year/tm_yday.
inline double julianDayAtMidnightUT(int year, int yday) {
  // Standard Gregorian expression evaluated at 1 January, then advanced by the day of year. The
  // truncation of the leap-year term is part of the formula, not a rounding accident.
  const double jan1 = 367.0 * year - floor(7.0 * year / 4.0) + 1721044.5;
  return jan1 + yday;
}

// NOAA sunrise equation. Accurate to well under a minute at temperate latitudes, which is far
// finer than anything this display does with the answer.
inline SolarTimes solarTimesForDay(int year, int yday, double latDeg, double lonDeg,
                                   int utcOffsetMin) {
  const double rad = M_PI / 180.0;
  const double jdMidnight = julianDayAtMidnightUT(year, yday);

  const double n = ceil(jdMidnight - 2451545.0 + 0.0008);
  // Longitude subtracts because east of Greenwich the sun crosses the meridian before UTC noon:
  // four minutes earlier per degree.
  const double meanSolarTime = n + 0.0009 - lonDeg / 360.0;

  const double meanAnomaly = fmod(357.5291 + 0.98560028 * meanSolarTime, 360.0);
  const double center = 1.9148 * sin(meanAnomaly * rad) + 0.0200 * sin(2 * meanAnomaly * rad) +
                        0.0003 * sin(3 * meanAnomaly * rad);
  const double eclipticLon = fmod(meanAnomaly + center + 180.0 + 102.9372, 360.0);

  const double transit = 2451545.0 + meanSolarTime + 0.0053 * sin(meanAnomaly * rad) -
                         0.0069 * sin(2 * eclipticLon * rad);

  const double sinDecl = sin(eclipticLon * rad) * sin(23.4397 * rad);
  const double cosDecl = cos(asin(sinDecl));

  // -0.833 deg puts the centre of the disc far enough below the horizon to account for refraction
  // and the solar radius, which is what published sunrise/sunset tables use.
  const double cosHourAngle =
      (sin(-0.833 * rad) - sin(latDeg * rad) * sinDecl) / (cos(latDeg * rad) * cosDecl);

  SolarTimes out;
  if (cosHourAngle > 1.0 || cosHourAngle < -1.0) return out;   // polar day or polar night

  const double hourAngle = acos(cosHourAngle) / rad;
  const double jdRise = transit - hourAngle / 360.0;
  const double jdSet = transit + hourAngle / 360.0;

  const double riseMin = (jdRise - jdMidnight) * 1440.0 + utcOffsetMin;
  const double setMin = (jdSet - jdMidnight) * 1440.0 + utcOffsetMin;

  out.sunriseMin = (int)lround(riseMin);
  out.sunsetMin = (int)lround(setMin);
  out.valid = true;
  return out;
}

// Applies the clamps. When the sun never sets or never rises the solar answer is meaningless, so
// this falls back to the most conservative pair the clamps allow: dim as late as permitted,
// brighten as late as permitted.
inline NightWindow nightWindowFor(const SolarTimes& solar) {
  NightWindow w;
  if (!solar.valid) return w;
  w.startMin = clampInt(solar.sunsetMin, NIGHT_START_EARLIEST_MIN, NIGHT_START_LATEST_MIN);
  w.endMin = solar.sunriseMin < NIGHT_END_EARLIEST_MIN ? NIGHT_END_EARLIEST_MIN : solar.sunriseMin;
  return w;
}

// Drives the dark theme, which cannot fade and so has to pick an instant. It flips where the fade
// begins, so the whole transition happens in one visual mode instead of changing halfway down.
inline bool isNightAt(int minuteOfDay, const NightWindow& w) {
  return minutesForward(w.startMin, minuteOfDay) < minutesForward(w.startMin, w.endMin);
}

inline int rampBetween(int from, int to, int elapsed, int span) {
  if (span <= 0) return to;
  return from + (int)lround((double)(to - from) * elapsed / span);
}

// Backlight level for a given local minute, ramped across both edges.
inline int brightnessAt(int minuteOfDay, const NightWindow& w) {
  const int nightLen = minutesForward(w.startMin, w.endMin);
  const int dayLen = 1440 - nightLen;

  const int sinceStart = minutesForward(w.startMin, minuteOfDay);
  if (sinceStart < nightLen) {
    // The ramp can never outrun its own window, so a freak short night still lands on the night
    // level rather than being cut off partway down.
    const int span = nightLen < BRIGHT_RAMP_MIN ? nightLen : BRIGHT_RAMP_MIN;
    if (sinceStart >= span) return BRIGHT_NIGHT_LEVEL;
    return rampBetween(BRIGHT_DAY_LEVEL, BRIGHT_NIGHT_LEVEL, sinceStart, span);
  }

  const int sinceEnd = minutesForward(w.endMin, minuteOfDay);
  const int span = dayLen < BRIGHT_RAMP_MIN ? dayLen : BRIGHT_RAMP_MIN;
  if (sinceEnd >= span) return BRIGHT_DAY_LEVEL;
  return rampBetween(BRIGHT_NIGHT_LEVEL, BRIGHT_DAY_LEVEL, sinceEnd, span);
}
