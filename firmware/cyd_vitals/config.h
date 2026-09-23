// config.h - every tunable of the monitor firmware, in one place.
//
// Thresholds are arbitrary and are not medical advice. This is a hobby receiver, not a safety
// monitor. Change values here rather than in the code that uses them.
//
// Protocol facts (frame layout, plausibility ranges, the rate-correction rules) are not tunables
// and stay in band_protocol.h. Scan-health and trace-gap timings stay with the logic they shape.
#pragma once

#include <stdint.h>

// ---- identity -----------------------------------------------------------------------------------

// Shown top-left of the live view. Change it before building for someone else.
constexpr char DISPLAY_NAME[] = "Oliwia";

// ---- panel: CYD ESP32-2432S028 with a TPM408-2.8 (ILI9342, 320x240) -----------------------------

constexpr int DISPLAY_ROTATION = 0;
constexpr int LCD_WIDTH = 320;
constexpr int LCD_HEIGHT = 240;
constexpr int LCD_PIN_SCLK = 14;
constexpr int LCD_PIN_MOSI = 13;
constexpr int LCD_PIN_MISO = 12;
constexpr int LCD_PIN_DC = 2;
constexpr int LCD_PIN_CS = 15;
constexpr int LCD_PIN_BACKLIGHT = 21;
constexpr uint32_t LCD_SPI_WRITE_HZ = 40'000'000;
constexpr uint32_t LCD_SPI_READ_HZ = 16'000'000;
constexpr uint32_t LCD_BACKLIGHT_PWM_HZ = 44'100;
constexpr int LCD_BACKLIGHT_PWM_CHANNEL = 7;

// ---- microSD on VSPI ----------------------------------------------------------------------------

constexpr int SD_PIN_SCK = 18;
constexpr int SD_PIN_MISO = 19;
constexpr int SD_PIN_MOSI = 23;
constexpr int SD_PIN_CS = 5;

// ---- XPT2046 touch, bit-banged on its own pins so it never contends with the SD bus -------------

constexpr int TOUCH_PIN_CLK = 25;
constexpr int TOUCH_PIN_MOSI = 32;
constexpr int TOUCH_PIN_MISO = 39;
constexpr int TOUCH_PIN_CS = 33;
constexpr int TOUCH_PIN_IRQ = 36;
// Raw ADC extents from a five-point calibration, mapped onto the panel.
constexpr int TOUCH_CAL_X0 = 170;
constexpr int TOUCH_CAL_X1 = 3840;
constexpr int TOUCH_CAL_Y0 = 320;
constexpr int TOUCH_CAL_Y1 = 3760;
// Raw pressure below this counts as no touch.
constexpr int TOUCH_PRESSURE_MIN = 250;
constexpr int TOUCH_SAMPLES = 5;

// ---- gestures -----------------------------------------------------------------------------------

// A swipe must cross more than half the 240 px height and be clearly more vertical than horizontal,
// so brushing the screen while moving the board cannot stop monitoring.
constexpr int SWIPE_MIN_DY = 130;
constexpr int TAP_MAX_MOVE = 20;   // px; anything that moves further is not a tap
constexpr uint32_t TAP_MAX_MS = 600;
constexpr uint32_t HOLD_COUNTDOWN_MS = 3000;   // press-and-hold for dismissal and self-test alike
constexpr uint32_t PLOT_AUTO_RETURN_MS = 10'000;
// The loop samples the panel faster while a finger is down so swipes track well.
constexpr uint32_t LOOP_DELAY_TOUCHING_MS = 15;
constexpr uint32_t LOOP_DELAY_IDLE_MS = 60;

// ---- warning thresholds: value colours and the alert bar ----------------------------------------

constexpr int HR_LOW = 90;
constexpr int HR_HIGH = 180;
constexpr int SPO2_LOW = 90;
constexpr uint32_t STALE_MS = 30'000;        // no packet for this long shows "--"
constexpr uint32_t READING_FRESH_MS = 3000;  // a new sequence is acted on only while this fresh
constexpr int SPO2_STALE_MIN = 20;           // minutes after which the alarm screen greys the SpO2

// ---- critical alarm -----------------------------------------------------------------------------
// Sized against a ten-minute decision window: a sustained rate at or above HR_CRIT means a hospital
// visit within roughly ten minutes, so a one-minute confirmation costs a tenth of it. HR_CANCEL
// equals HR_HIGH and HR_SUSTAIN_LOW equals HR_LOW so the six numbers read as one scale.

constexpr int HR_CRIT = 200;           // at or above: arms the fast alarm
constexpr int HR_SUSTAIN = 190;        // the last reading of the window must be at or above this
constexpr int HR_CANCEL = 180;         // a reading below this abandons the window
constexpr int HR_COLLAPSE_FROM = 170;  // previous reading at or above this ...
constexpr int HR_COLLAPSE_TO = 60;     // ... and this one at or below it alarms at once
constexpr int HR_RAIL = 255;           // the uint8_t ceiling; shown as ">=255"

constexpr int HR_CRIT_LOW = 80;        // at or below: arms the slow alarm
constexpr int HR_SUSTAIN_LOW = 90;     // the last reading of the window must be at or below this
constexpr int HR_CANCEL_LOW = 100;     // a reading above this abandons the window

constexpr int CRIT_MIN_HIGH = 2;                   // readings beyond crit needed in one window
constexpr int CRIT_MIN_CORRECTED = 3;              // ... when the window holds beat-interval readings
constexpr uint32_t CRIT_CONFIRM_MS = 60'000;
constexpr uint32_t CRIT_CONFIRM_CORRECTED_MS = 120'000;
constexpr uint32_t ALARM_SNOOZE_MS = 600'000;      // quiet after a dismissal, so the board can be carried
constexpr uint32_t ALARM_FLASH_MS = 500;           // half-period of the perimeter flash
// A minute, not ten seconds: the heart rate updates about every 20 s, so a shorter test never shows
// the trace move. A three-second hold ends it early.
constexpr int SELFTEST_DURATION_S = 60;

// ---- Bluetooth scan -----------------------------------------------------------------------------
// Continuous (window equals interval). More duty does not buy more data - the band rebroadcasts one
// measurement for ~20 s - but this runs from a powerbank, and many powerbanks switch off when the
// load drops below a floor; a receiver that never sleeps stays above it. Measured on this board with
// the display and SD running: 30% duty produced 13 s gaps, close to STALE_MS. Do not lower the
// window without re-measuring. See README, "Power draw".
constexpr uint32_t SCAN_INTERVAL_MS = 100;
constexpr uint32_t SCAN_WINDOW_MS = 100;

// ---- clock --------------------------------------------------------------------------------------

constexpr char TZ_INFO[] = "CET-1CEST,M3.5.0,M10.5.0/3";   // Europe/Warsaw
constexpr char NTP_SERVER_1[] = "pool.ntp.org";
constexpr char NTP_SERVER_2[] = "time.nist.gov";
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15'000;
constexpr uint32_t NTP_TIMEOUT_MS = 15'000;

// ---- day/night backlight ------------------------------------------------------------------------
// The schedule follows sunset and sunrise for the site, clamped so the solstices cannot dim the
// screen mid-afternoon or wake the room at 04:14. The solar maths is in day_night.h.

constexpr double SITE_LATITUDE_DEG = 52.23;   // Warsaw
constexpr double SITE_LONGITUDE_DEG = 21.01;  // positive east
constexpr int NIGHT_START_EARLIEST_MIN = 19 * 60;  // never dim before 19:00
constexpr int NIGHT_START_LATEST_MIN = 21 * 60;    // always dim by 21:00
constexpr int NIGHT_END_EARLIEST_MIN = 7 * 60;     // never brighten before 07:00
// PWM levels. Current draw tracks duty closely, so these are close to their percentages.
constexpr int BRIGHT_DAY_LEVEL = 255;
constexpr int BRIGHT_NIGHT_LEVEL = 26;   // about a tenth of full
constexpr int BRIGHT_ALARM_LEVEL = 255;  // the alarm and export screens ignore the schedule
// Both edges fade over this many minutes rather than stepping.
constexpr int BRIGHT_RAMP_MIN = 60;
constexpr uint32_t BACKLIGHT_REFRESH_MS = 10'000;
// Test aid: true forces the night theme and level regardless of the clock.
constexpr bool FORCE_NIGHT = false;

// ---- data export: the board as its own access point ---------------------------------------------

constexpr char AP_SSID[] = "BabyVitals";
constexpr char AP_PASS[] = "babyvitals";   // WPA2 needs at least 8 characters; shown on screen
constexpr uint32_t EXPORT_TIMEOUT_MS = 600'000;   // resume monitoring by itself after 10 minutes
constexpr uint32_t EXPORT_COUNTDOWN_REFRESH_MS = 1000;

// ---- firmware update over the air: maintenance mode on the home network -------------------------

constexpr char OTA_HOSTNAME[] = "babyvitals";       // also reachable as babyvitals.local
constexpr uint16_t OTA_PORT = 3232;
constexpr uint32_t OTA_MODE_TIMEOUT_MS = 600'000;   // resume monitoring by itself after 10 minutes
constexpr uint32_t OTA_COUNTDOWN_REFRESH_MS = 1000;
// A freshly installed firmware is marked good once the scan is running and the loop has survived
// this long. A crash before then rolls back to the previous firmware (firmware_health.h).
constexpr uint32_t OTA_HEALTHY_AFTER_MS = 60'000;
