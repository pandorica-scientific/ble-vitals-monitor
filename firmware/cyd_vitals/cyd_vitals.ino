// cyd_vitals.ino - Baby Sensor Relax passive monitor and logger for the CYD ESP32-2432S028.
//
// Receive-only: the board listens to the advertisements the wristband already broadcasts. It never
// connects, pairs or transmits to the wristband or its base, so the official system is untouched.
//
// This file is the glue. Pure logic lives in the natively tested headers, drawing in the *_render.h
// headers, every tunable in config.h. Sections, in order:
//
//   readings   the latest decoded frame, shared between the BLE task and the loop
//   radio      passive BLE scan, its health classification and rate-limited recovery
//   clock      local time helpers; WiFi + NTP once per power cycle, then a soft reboot to BLE-only
//   storage    daily CSV, /alerts.csv, /boot.log and /contacts.txt on the microSD card
//   backlight  solar day/night schedule and the dark theme
//   touch      XPT2046 sampling (gestures are decoded by gesture.h and hold_gesture.h)
//   export     swipe up: the board becomes an access point serving the CSVs; monitoring pauses
//   update     swipe down: the board joins the home network and accepts a firmware upload
//   views      live, 24-hour plot, alarm screen and the alarm self-test
//   alarms     feeding both critical alarm machines
//   setup, loop
//
// Thresholds are arbitrary and not medical advice. Hobby device, not a safety monitor.

#define LGFX_USE_V1
#include <ArduinoOTA.h>
#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <LovyanGFX.hpp>
#include <SD.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <time.h>

#include "alarm_render.h"
#include "alert_log.h"
#include "app_html.h"
#include "band_protocol.h"
#include "config.h"
#include "contacts.h"
#include "critical_alarm.h"
#include "day_bins.h"
#include "day_night.h"
#include "display.h"
#include "export_render.h"
#include "firmware_health.h"
#include "gesture.h"
#include "history.h"
#include "hold_gesture.h"
#include "live_render.h"
#include "live_render_key.h"
#include "maintenance_render.h"
#include "ota_password.h"
#include "plot_render.h"
#include "radio_health.h"
#include "trace_window.h"
#include "vitals_csv.h"
#include "wifi_failover.h"

// =================================================================================================
// Sketch-local types
// =================================================================================================
// Defined before any function on purpose: the Arduino preprocessor inserts generated prototypes for
// every function ahead of the first definition, so a type used in a signature has to exist by then.

enum class View : uint8_t { LIVE, PLOT, EXPORT, MAINTENANCE, ALARM };

// Outcome of the press-and-hold on the HEART cell that starts the alarm self-test.
enum class HeartHold : uint8_t { NONE, COUNTING, COMPLETED };

// Bluetooth reception counters, written by the BLE task and read by the loop.
struct RadioRuntime {
  bool scanStarted = false;
  uint32_t scanStartedMs = 0;
  bool anySeen = false;
  uint32_t lastAnyMs = 0;
  bool bandSeen = false;
  uint32_t lastBandMs = 0;
  uint32_t totalAdvertisements = 0;
  uint32_t bandAdvertisements = 0;
  bool restartAttempted = false;
  uint32_t lastRestartAttemptMs = 0;
  uint32_t restartCount = 0;
};

struct WifiCredential {
  String ssid;
  String password;
};

// The alarm view repaints its vitals and trace only when one of these changes.
struct AlarmRenderKey {
  int sequence = -1;
  int effectiveHeartRate = 0;
  int heartRate = 0;
  int highAlarmHeartRate = 0;
  int oxygen = 0;
  bool stale = false;
  RadioState radioState = RadioState::STARTING;
};

bool operator==(const AlarmRenderKey& a, const AlarmRenderKey& b) {
  return a.sequence == b.sequence && a.effectiveHeartRate == b.effectiveHeartRate &&
         a.heartRate == b.heartRate && a.highAlarmHeartRate == b.highAlarmHeartRate &&
         a.oxygen == b.oxygen && a.stale == b.stale && a.radioState == b.radioState;
}

// =================================================================================================
// Shared state
// =================================================================================================

CydPanel g_tft;
Screen g_screen{g_tft, Layout{}, DAY_PALETTE};
SPIClass g_sdSpi(VSPI);

bool g_sdReady = false;
bool g_timeReady = false;

// RTC memory survives the deliberate soft reboot after the NTP sync, a watchdog reset and a panic,
// but not a power cut. That is exactly the distinction the boot log and the alarm latch need.
RTC_DATA_ATTR bool g_syncedThisPower = false;
// The real reason this power cycle started. The cold boot has no clock yet and cannot write a
// timestamped line; by the time the second boot can, esp_reset_reason() only reports our restart.
RTC_DATA_ATTR esp_reset_reason_t g_originalResetReason = ESP_RST_UNKNOWN;
// The latch outlives a reboot here. The onset is also written to /alerts.csv the moment it happens
// and reconciled at boot, which covers the power-cut case.
RTC_DATA_ATTR AlarmMachine g_alarmHigh;
RTC_DATA_ATTR AlarmMachine g_alarmLow;

const AlarmConfig ALARM_HIGH_CFG{HR_CRIT,       HR_SUSTAIN,      HR_CANCEL,
                                 HR_COLLAPSE_FROM, HR_COLLAPSE_TO, CRIT_MIN_HIGH,
                                 CRIT_CONFIRM_MS,  ALARM_SNOOZE_MS, false,
                                 CRIT_MIN_CORRECTED, CRIT_CONFIRM_CORRECTED_MS};
// The collapse thresholds are carried but unused: that rule is high-side only (critical_alarm.h).
// lowSide = true reads every other comparison the other way up.
const AlarmConfig ALARM_LOW_CFG{HR_CRIT_LOW,      HR_SUSTAIN_LOW,  HR_CANCEL_LOW,
                                HR_COLLAPSE_FROM, HR_COLLAPSE_TO,  CRIT_MIN_HIGH,
                                CRIT_CONFIRM_MS,  ALARM_SNOOZE_MS, true,
                                CRIT_MIN_CORRECTED, CRIT_CONFIRM_CORRECTED_MS};

History g_history;   // the last hour, for the sparklines and the alarm trace
DayBins g_dayBins;   // today, for the 24-hour chart
Contacts g_contacts;

View g_view = View::LIVE;
DayMetric g_plotMetric = DayMetric::HEART_RATE;
int g_plotBinMinutes = 60;   // 60, 30 or 15
uint32_t g_lastTouchMs = 0;

LiveRenderKey g_lastRenderKey;
bool g_renderKeyValid = false;
bool g_selfTest = false;
uint32_t g_selfTestStartMs = 0;
uint32_t g_spo2StampEpoch = 0;   // when the last non-zero SpO2 arrived, for its age on the alarm screen
uint32_t g_exportStartMs = 0;
uint32_t g_maintenanceStartMs = 0;
bool g_otaUploading = false;   // a transfer is in progress: no timeout, no gestures

GestureTracker g_gesture;
HoldState g_hold;
// The one touch sample taken per loop pass, shared by the gesture decoder and the hold countdown.
bool g_touchDown = false;
int g_touchX = 0;
int g_touchY = 0;

// Whichever machine is latched owns the screen. If somehow both are, the fast one wins: a rate
// above 200 needs a hospital sooner than one below 80.
AlarmMachine* firingAlarm() {
  if (g_alarmHigh.state == AlarmState::ALARM) return &g_alarmHigh;
  if (g_alarmLow.state == AlarmState::ALARM) return &g_alarmLow;
  return nullptr;
}

// =================================================================================================
// Readings: written by the BLE callback, read by the loop
// =================================================================================================

ReadingSnapshot g_reading;
RateContext g_rateContext;   // the last five minutes of agreeing readings (band_protocol.h)
portMUX_TYPE g_readingMux = portMUX_INITIALIZER_UNLOCKED;

ReadingSnapshot readSnapshot() {
  portENTER_CRITICAL(&g_readingMux);
  const ReadingSnapshot copy = g_reading;
  portEXIT_CRITICAL(&g_readingMux);
  return copy;
}

void publishReading(const BandReading& frame, int rssi, uint32_t nowMs) {
  portENTER_CRITICAL(&g_readingMux);
  g_reading = mergeBandReading(g_reading, frame, rssi, nowMs, g_rateContext);
  portEXIT_CRITICAL(&g_readingMux);
}

// The display holds the last valid skin temperature, as the official app does, instead of "--".
void holdSkinTemperature(float skinC) {
  if (skinC < BAND_SKIN_MIN_C || skinC > BAND_SKIN_MAX_C) return;
  portENTER_CRITICAL(&g_readingMux);
  g_reading.skinC = skinC;
  g_reading.skinValid = true;
  portEXIT_CRITICAL(&g_readingMux);
}

bool readingIsFresh(const ReadingSnapshot& r, uint32_t nowMs) {
  return r.lastPacketMs != 0 && static_cast<uint32_t>(nowMs - r.lastPacketMs) < READING_FRESH_MS;
}

bool readingIsStale(const ReadingSnapshot& r, uint32_t nowMs) {
  return r.lastPacketMs == 0 || static_cast<uint32_t>(nowMs - r.lastPacketMs) > STALE_MS;
}

// =================================================================================================
// Radio: passive BLE scan
// =================================================================================================

RadioRuntime g_radio;
portMUX_TYPE g_radioMux = portMUX_INITIALIZER_UNLOCKED;
BLEScan* g_bleScanner = nullptr;

RadioRuntime readRadioRuntime() {
  portENTER_CRITICAL(&g_radioMux);
  const RadioRuntime copy = g_radio;
  portEXIT_CRITICAL(&g_radioMux);
  return copy;
}

void noteAnyAdvertisement(uint32_t nowMs) {
  portENTER_CRITICAL(&g_radioMux);
  g_radio.anySeen = true;
  g_radio.lastAnyMs = nowMs;
  ++g_radio.totalAdvertisements;
  portEXIT_CRITICAL(&g_radioMux);
}

void noteBandAdvertisement(uint32_t nowMs) {
  portENTER_CRITICAL(&g_radioMux);
  g_radio.bandSeen = true;
  g_radio.lastBandMs = nowMs;
  ++g_radio.bandAdvertisements;
  portEXIT_CRITICAL(&g_radioMux);
}

bool startBleScan(uint32_t nowMs) {
  // Passive: never transmit a scan request. Required by the receive-only rule, and everything the
  // board decodes is in the advertisement itself.
  g_bleScanner->setActiveScan(false);
  g_bleScanner->setInterval(SCAN_INTERVAL_MS);
  g_bleScanner->setWindow(SCAN_WINDOW_MS);
  const bool started = g_bleScanner->start(0, nullptr, false);
  portENTER_CRITICAL(&g_radioMux);
  g_radio.scanStarted = started;
  g_radio.scanStartedMs = nowMs;
  portEXIT_CRITICAL(&g_radioMux);
  Serial.printf("[ble] scan start %s\n", started ? "OK" : "FAILED");
  return started;
}

RadioState classifyRadio(uint32_t nowMs, const RadioRuntime& radio) {
  RadioHealthInput input{};
  input.nowMs = nowMs;
  input.scanStarted = radio.scanStarted;
  input.scanStartedMs = radio.scanStartedMs;
  input.anySeen = radio.anySeen;
  input.lastAnyMs = radio.lastAnyMs;
  input.bandSeen = radio.bandSeen;
  input.lastBandMs = radio.lastBandMs;
  return classifyRadioHealth(input);
}

// Logs every state change and restarts a silent scanner, rate-limited by radio_health.h.
RadioState serviceRadioRecovery(uint32_t nowMs) {
  const RadioRuntime radio = readRadioRuntime();
  const RadioState state = classifyRadio(nowMs, radio);

  static bool havePrevious = false;
  static RadioState previous = RadioState::STARTING;
  if (!havePrevious || state != previous) {
    Serial.printf("[ble] state=%s total=%lu band=%lu restarts=%lu\n", radioStateName(state),
                  static_cast<unsigned long>(radio.totalAdvertisements),
                  static_cast<unsigned long>(radio.bandAdvertisements),
                  static_cast<unsigned long>(radio.restartCount));
    previous = state;
    havePrevious = true;
  }

  if (!shouldRestartScan(state, radio.restartAttempted, nowMs, radio.lastRestartAttemptMs)) {
    return state;
  }
  portENTER_CRITICAL(&g_radioMux);
  g_radio.restartAttempted = true;
  g_radio.lastRestartAttemptMs = nowMs;
  ++g_radio.restartCount;
  portEXIT_CRITICAL(&g_radioMux);

  if (radio.scanStarted) g_bleScanner->stop();
  delay(20);
  startBleScan(millis());
  return classifyRadio(millis(), readRadioRuntime());
}

class BandScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice device) override {
    const uint32_t nowMs = millis();
    noteAnyAdvertisement(nowMs);
    if (!device.haveManufacturerData()) return;
    const String manufacturer = device.getManufacturerData();
    BandReading decoded{};
    if (!decodeBandFrame(reinterpret_cast<const uint8_t*>(manufacturer.c_str()),
                         manufacturer.length(), decoded)) {
      return;
    }
    noteBandAdvertisement(nowMs);
    publishReading(decoded, device.getRSSI(), nowMs);
  }
};

// =================================================================================================
// Clock
// =================================================================================================

// tm_year counts from 1900. Anything before 2020 is the unset epoch, not a real time.
constexpr int CLOCK_SET_MIN_TM_YEAR = 120;

bool readLocalClock(struct tm& out) {
  time_t now;
  time(&now);
  localtime_r(&now, &out);
  return out.tm_year > CLOCK_SET_MIN_TM_YEAR;
}

// 0 means "no usable timestamp". The alarm trace then falls back to a positional axis and says so,
// rather than placing samples at epoch zero and drawing a confident wrong time axis.
uint32_t nowEpochOrZero() {
  struct tm tm{};
  if (!readLocalClock(tm)) return 0;
  return static_cast<uint32_t>(mktime(&tm));
}

// Rows in today's CSV carry a time of day; combined with today's date they land on the same axis as
// live readings. Seconds matter: readings arrive about every 20 s, so quantising to the minute would
// stack several on one x position and turn a real dropout into an apparent one.
uint32_t epochForTodayAt(int hh, int mm, int ss) {
  struct tm tm{};
  if (!readLocalClock(tm)) return 0;
  tm.tm_hour = hh;
  tm.tm_min = mm;
  tm.tm_sec = ss;
  return static_cast<uint32_t>(mktime(&tm));
}

// "HH:MM", or an empty string until the clock is set.
void clockText(char* out, size_t cap) {
  struct tm tm{};
  if (cap == 0) return;
  out[0] = '\0';
  if (readLocalClock(tm)) strftime(out, cap, "%H:%M", &tm);
}

int minuteOfDay() {
  struct tm tm{};
  if (!readLocalClock(tm)) return -1;
  return tm.tm_hour * 60 + tm.tm_min;
}

// ---- WiFi + NTP: once per power cycle, then off for good ----------------------------------------

bool loadWifiCredential(WifiSlot slot, WifiCredential& out) {
  File file = SD.open(wifiCredentialPath(slot));
  if (!file) return false;
  out.ssid = file.readStringUntil('\n');
  out.password = file.readStringUntil('\n');
  file.close();
  out.ssid.trim();
  out.password.trim();
  return out.ssid.length() > 0;
}

// Loads one slot's credentials and connects. Leaves the connection up on success. Credentials are
// never logged.
bool connectWifiSlot(WifiSlot slot) {
  WifiCredential credential;
  if (!loadWifiCredential(slot, credential)) {
    Serial.printf("[wifi] %s unavailable\n", wifiSlotLabel(slot));
    return false;
  }
  Serial.printf("[wifi] trying %s\n", wifiSlotLabel(slot));
  WiFi.begin(credential.ssid.c_str(), credential.password.c_str());
  credential.password = "";
  const uint32_t startedMs = millis();
  while (WiFi.status() != WL_CONNECTED &&
         static_cast<uint32_t>(millis() - startedMs) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.printf("[wifi] %s failed\n", wifiSlotLabel(slot));
  WiFi.disconnect(false, false);
  delay(100);
  return false;
}

// Waits for NTP on the current connection.
bool syncNtp() {
  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
  const uint32_t startedMs = millis();
  struct tm localTime{};
  while (static_cast<uint32_t>(millis() - startedMs) < NTP_TIMEOUT_MS) {
    if (getLocalTime(&localTime, 200) && localTime.tm_year > CLOCK_SET_MIN_TM_YEAR) return true;
    delay(50);
  }
  return false;
}

// One slot's attempt at setting the clock. The connection is dropped either way: at boot the
// network is only borrowed for the time.
bool tryWifiSlotForTime(WifiSlot slot) {
  if (!connectWifiSlot(slot)) return false;
  const bool synced = syncNtp();
  Serial.printf("[wifi] %s %s\n", wifiSlotLabel(slot), synced ? "time-synced" : "ntp failed");
  WiFi.disconnect(false, false);
  delay(100);
  return synced;
}

void shutdownWifi() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  esp_wifi_deinit();
}

bool syncTimeOverWifi() {
  if (!g_sdReady) return false;
  WiFi.mode(WIFI_STA);
  const bool synced = runWifiFailover([](WifiSlot slot) { return tryWifiSlotForTime(slot); });
  shutdownWifi();
  return synced;
}

// =================================================================================================
// Storage: microSD card
// =================================================================================================

constexpr char VITALS_CSV_NAME_FORMAT[] = "/vitals_%Y-%m-%d.csv";
constexpr char ALERTS_CSV_PATH[] = "/alerts.csv";
constexpr char ALERTS_CSV_HEADER[] = "timestamp,event,hr_bpm,spo2_pct,detail";
constexpr char BOOT_LOG_PATH[] = "/boot.log";
constexpr char CONTACTS_PATH[] = "/contacts.txt";
constexpr char TIMESTAMP_FORMAT[] = "%F %T";   // "YYYY-MM-DD HH:MM:SS"

bool initSd() {
  g_sdSpi.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
  g_sdReady = SD.begin(SD_PIN_CS, g_sdSpi) && SD.cardType() != CARD_NONE;
  Serial.println(g_sdReady ? "[sd] OK" : "[sd] FAILED");
  return g_sdReady;
}

// One definition of the daily filename: the writer and the reader must never disagree about which
// file "today" is.
void vitalsCsvName(char* out, size_t cap, const struct tm& tm) {
  strftime(out, cap, VITALS_CSV_NAME_FORMAT, &tm);
}

void logVitalsRow(const ReadingSnapshot& r) {
  if (!g_sdReady || !g_timeReady) return;
  struct tm tm{};
  if (!readLocalClock(tm)) return;
  char name[32];
  vitalsCsvName(name, sizeof(name), tm);
  const bool isNew = !SD.exists(name);
  File file = SD.open(name, FILE_APPEND);
  if (!file) return;
  if (isNew) file.println(VITALS_CSV_HEADER);
  char timestamp[24];
  strftime(timestamp, sizeof(timestamp), TIMESTAMP_FORMAT, &tm);
  // hr_bpm is the band's own byte and hr_eff is what the screen, plots and alarms used. A day that
  // started before this firmware keeps its older header and gains wider rows; parseVitalsRow()
  // reads both.
  char row[80];
  formatVitalsRow(row, sizeof(row), timestamp, r.heartRate, r.oxygenSaturation, r.skinC,
                  r.skinValid, r.beatMs, r.effectiveHeartRate);
  file.println(row);
  file.close();
}

// Append-only: rewriting a row in place on an SD card is not crash-safe, and a brownout mid-episode
// must still leave a readable file with an open episode in it.
void logAlert(const char* event, int hr, int spo2, const char* detail) {
  if (!g_sdReady) return;
  struct tm tm{};
  if (!readLocalClock(tm)) return;
  char timestamp[24];
  strftime(timestamp, sizeof(timestamp), TIMESTAMP_FORMAT, &tm);
  const bool isNew = !SD.exists(ALERTS_CSV_PATH);
  File file = SD.open(ALERTS_CSV_PATH, FILE_APPEND);
  if (!file) return;
  if (isNew) file.println(ALERTS_CSV_HEADER);
  char row[128];
  formatAlertRow(row, sizeof(row), timestamp, event, hr, spo2, detail);
  file.println(row);
  file.close();
}

// The repository ships no /contacts.txt: one family's hospital numbers must never end up on a
// stranger's screen.
void loadContacts() {
  g_contacts = Contacts{};
  if (!g_sdReady) return;
  File file = SD.open(CONTACTS_PATH, FILE_READ);
  if (!file) return;
  char buf[128];
  const size_t n = file.readBytes(buf, sizeof(buf) - 1);
  buf[n] = '\0';
  file.close();
  parseContacts(buf, g_contacts);
}

// The over-the-air update password. Like the WiFi credentials it lives on the card, never in code.
bool loadOtaPassword(char* out, size_t cap) {
  if (!g_sdReady) return false;
  File file = SD.open(OTA_PASSWORD_PATH, FILE_READ);
  if (!file) return false;
  char buf[128];
  const size_t n = file.readBytes(buf, sizeof(buf) - 1);
  buf[n] = '\0';
  file.close();
  const bool ok = parseOtaPassword(buf, out, cap);
  memset(buf, 0, sizeof(buf));
  return ok;
}

// An ONSET with no later DISMISS means the board died mid-episode. Coming back up silent would be
// the worst possible behaviour, so the alarm is re-entered.
void reconcileOpenEpisode() {
  if (!g_sdReady || !g_timeReady) return;
  File file = SD.open(ALERTS_CSV_PATH, FILE_READ);
  if (!file) return;
  OpenEpisode open{};
  while (file.available()) {
    const String line = file.readStringUntil('\n');
    if (line.length() < 20) continue;
    struct tm tm{};
    if (strptime(line.c_str(), "%Y-%m-%d %H:%M:%S", &tm) == nullptr) continue;
    reconcileAlertLine(line.c_str(), static_cast<uint32_t>(mktime(&tm)), open);
  }
  file.close();
  if (!open.open || firingAlarm() != nullptr) return;

  AlarmMachine& m = open.low ? g_alarmLow : g_alarmHigh;
  m.state = AlarmState::ALARM;
  m.cause = open.low ? AlarmCause::CONFIRMED_LOW
            : open.beat ? AlarmCause::CONFIRMED_HIGH_BEAT
                        : AlarmCause::CONFIRMED_HIGH;
  m.onsetEpoch = open.onsetEpoch;
  m.resolved = false;
  logAlert("BOOT_RESUME", 0, 0, "recovered from card");
}

// Refills the 24-hour bins and the one-hour history from today's file, so the charts survive a
// power cycle.
void loadTodayFromCsv() {
  if (!g_sdReady || !g_timeReady) return;
  struct tm tm{};
  if (!readLocalClock(tm)) return;
  char name[32];
  vitalsCsvName(name, sizeof(name), tm);
  File file = SD.open(name);
  if (!file) {
    Serial.println("[csv] no file to load");
    return;
  }
  file.readStringUntil('\n');   // header
  int rows = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    VitalsRow row;
    if (!parseVitalsRow(line.c_str(), row)) continue;
    // The corrected rate where the file has one, the raw byte for rows written before it existed.
    const int hr = vitalsRowHistoryHr(row);
    dayBinsAdd(g_dayBins, hr, row.spo2, row.hh * 60 + row.mm);
    historyPush(g_history, hr, row.spo2, epochForTodayAt(row.hh, row.mm, row.ss));
    if (row.skinValid) holdSkinTemperature(row.skinC);
    ++rows;
  }
  file.close();
  const ReadingSnapshot held = readSnapshot();
  Serial.printf("[csv] loaded %d rows (held skin %.1f valid=%d)\n", rows,
                static_cast<double>(held.skinC), held.skinValid);
}

// Why the board restarted, appended to its own file so the CSV format stays untouched. With no
// current meter on the board this is the only evidence separating the causes of a dead screen in
// the morning: POWERON is a cold boot or supply interruption, BROWNOUT a supply that fell too low,
// PANIC or a watchdog a firmware fault. One POWERON when it is plugged in is normal.
void logBoot() {
  if (!g_sdReady || !g_timeReady) return;
  const char* reason = "OTHER";
  switch (g_originalResetReason) {
    case ESP_RST_POWERON: reason = "POWERON"; break;
    case ESP_RST_BROWNOUT: reason = "BROWNOUT"; break;
    case ESP_RST_SW: reason = "SW"; break;
    case ESP_RST_PANIC: reason = "PANIC"; break;
    case ESP_RST_INT_WDT: reason = "INT_WDT"; break;
    case ESP_RST_TASK_WDT: reason = "TASK_WDT"; break;
    case ESP_RST_WDT: reason = "WDT"; break;
    case ESP_RST_DEEPSLEEP: reason = "DEEPSLEEP"; break;
    case ESP_RST_EXT: reason = "EXT"; break;   // EN pin pulled low: a flash or serial reset
    default: break;
  }
  File file = SD.open(BOOT_LOG_PATH, FILE_APPEND);
  if (!file) return;
  struct tm tm{};
  char timestamp[24] = "?";
  if (readLocalClock(tm)) strftime(timestamp, sizeof(timestamp), TIMESTAMP_FORMAT, &tm);
  // The numeric code goes in too: the enum has more members than are worth naming here.
  // The running slot says which of the two firmware images booted, the first question after an
  // over-the-air update.
  const char* slot = runningPartitionLabel();
  file.printf("%s,%s,%d,%s\n", timestamp, reason, static_cast<int>(g_originalResetReason), slot);
  file.close();
  Serial.printf("[boot] %s reset=%s(%d) slot=%s\n", timestamp, reason,
                static_cast<int>(g_originalResetReason), slot);
}

// =================================================================================================
// Backlight and theme
// =================================================================================================

// The night window moves once a day and the loop asks about it every few seconds, so it is computed
// on the first call of each new date and cached. The UTC offset comes from the configured timezone,
// so the March and October changeovers need no special case: the window shifts with the clocks.
NightWindow currentNightWindow(const struct tm& local, time_t now) {
  static NightWindow cached;
  static int cachedYday = -1;
  static int cachedYear = -1;
  if (local.tm_yday != cachedYday || local.tm_year != cachedYear) {
    struct tm utc{};
    gmtime_r(&now, &utc);
    cached = nightWindowFor(solarTimesForDay(local.tm_year + 1900, local.tm_yday,
                                             SITE_LATITUDE_DEG, SITE_LONGITUDE_DEG,
                                             utcOffsetMinutes(local, utc)));
    cachedYday = local.tm_yday;
    cachedYear = local.tm_year;
  }
  return cached;
}

// The theme and the backlight need the same two facts, the minute of the local day and that day's
// night window, so they are read together and cannot disagree.
bool localDayState(int& minuteOut, NightWindow& windowOut) {
  time_t now;
  time(&now);
  struct tm local{};
  localtime_r(&now, &local);
  if (local.tm_year <= CLOCK_SET_MIN_TM_YEAR) return false;
  minuteOut = local.tm_hour * 60 + local.tm_min;
  windowOut = currentNightWindow(local, now);
  return true;
}

// The dark theme cannot fade, so it flips in one step where the backlight starts fading down.
// Without a valid clock the day look is the safe default.
bool isNight() {
  if (FORCE_NIGHT) return true;
  if (!g_timeReady) return false;
  int minute;
  NightWindow window;
  if (!localDayState(minute, window)) return false;
  return isNightAt(minute, window);
}

// An unknown clock means full brightness, never a dark screen nobody asked for.
int wantedBrightness() {
  if (FORCE_NIGHT) return BRIGHT_NIGHT_LEVEL;
  int minute;
  NightWindow window;
  if (!g_timeReady || !localDayState(minute, window)) return BRIGHT_DAY_LEVEL;
  return brightnessAt(minute, window);
}

// The single owner of the backlight PWM; only writes on a change.
void setBacklight(int level) {
  static int current = -1;
  if (level == current) return;
  g_tft.setBrightness(level);
  current = level;
}

void applyBrightness() { setBacklight(wantedBrightness()); }

void drawCurrentPlot() { drawDayPlot(g_screen, g_dayBins, g_plotMetric, g_plotBinMinutes); }

// Swaps the chrome palette when the night window opens or closes, then repaints.
void applyTheme() {
  static int current = -1;
  const int night = isNight() ? 1 : 0;
  if (night == current) return;
  current = night;
  g_screen.palette = paletteFor(night != 0);
  g_renderKeyValid = false;
  g_tft.fillScreen(g_screen.palette.bg);
  if (g_view == View::PLOT) drawCurrentPlot();
}

// =================================================================================================
// Touch: XPT2046, bit-banged
// =================================================================================================

void initTouchPins() {
  pinMode(TOUCH_PIN_CLK, OUTPUT);
  pinMode(TOUCH_PIN_MOSI, OUTPUT);
  pinMode(TOUCH_PIN_CS, OUTPUT);
  pinMode(TOUCH_PIN_MISO, INPUT);
  pinMode(TOUCH_PIN_IRQ, INPUT);
  digitalWrite(TOUCH_PIN_CS, HIGH);
  digitalWrite(TOUCH_PIN_CLK, LOW);
}

// One 12-bit conversion. 0xB0 reads pressure, 0x90 the X axis, 0xD0 the Y axis.
uint16_t touchCommand(uint8_t cmd) {
  digitalWrite(TOUCH_PIN_CS, LOW);
  for (int i = 7; i >= 0; --i) {
    digitalWrite(TOUCH_PIN_MOSI, (cmd >> i) & 1);
    digitalWrite(TOUCH_PIN_CLK, HIGH);
    digitalWrite(TOUCH_PIN_CLK, LOW);
  }
  uint16_t value = 0;
  digitalWrite(TOUCH_PIN_CLK, HIGH);   // busy cycle
  digitalWrite(TOUCH_PIN_CLK, LOW);
  for (int i = 11; i >= 0; --i) {
    digitalWrite(TOUCH_PIN_CLK, HIGH);
    value |= static_cast<uint16_t>(digitalRead(TOUCH_PIN_MISO) << i);
    digitalWrite(TOUCH_PIN_CLK, LOW);
  }
  digitalWrite(TOUCH_PIN_CS, HIGH);
  return value;
}

// Averaged screen coordinates of the current touch, or false when nothing is pressing.
bool readTouch(int& x, int& y) {
  if (touchCommand(0xB0) < TOUCH_PRESSURE_MIN) return false;
  uint32_t sumX = 0;
  uint32_t sumY = 0;
  for (int i = 0; i < TOUCH_SAMPLES; ++i) {
    sumX += touchCommand(0x90);
    sumY += touchCommand(0xD0);
  }
  const Layout& L = g_screen.layout;
  x = constrain(static_cast<int>(map(sumX / TOUCH_SAMPLES, TOUCH_CAL_X0, TOUCH_CAL_X1, 0, L.w)),
                0, L.w - 1);
  y = constrain(static_cast<int>(map(sumY / TOUCH_SAMPLES, TOUCH_CAL_Y0, TOUCH_CAL_Y1, 0, L.h)),
                0, L.h - 1);
  return true;
}

// =================================================================================================
// Export mode: the board as an access point with a small read-only HTTP server
// =================================================================================================

WebServer* g_http = nullptr;

// A whitelist, never a path mapping: /wifi.txt on the same card holds the home network password in
// plain text, and this access point is reachable by anyone in the room while it is up.
bool exportAllowed(const String& path) {
  return path.startsWith("/vitals_") && path.endsWith(".csv") && path.indexOf("..") < 0;
}

// JSON index of the available days, so the page can offer a range without guessing filenames.
void handleDays() {
  String json = "[";
  File dir = SD.open("/");
  if (dir) {
    for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      String name = entry.name();
      if (!name.startsWith("/")) name = "/" + name;
      if (exportAllowed(name)) {
        if (json.length() > 1) json += ",";
        json += "{\"f\":\"" + name + "\",\"n\":" + String(static_cast<uint32_t>(entry.size())) + "}";
      }
      entry.close();
    }
    dir.close();
  }
  json += "]";
  g_http->send(200, "application/json", json);
}

void handleFile() {
  const String path = g_http->uri();
  if (!exportAllowed(path)) {
    g_http->send(404, "text/plain", "not found");
    return;
  }
  File file = SD.open(path);
  if (!file) {
    g_http->send(404, "text/plain", "not found");
    return;
  }
  g_http->streamFile(file, "text/csv");
  file.close();
}

uint32_t exportSecondsLeft() {
  const uint32_t elapsed = millis() - g_exportStartMs;
  return elapsed >= EXPORT_TIMEOUT_MS ? 0 : (EXPORT_TIMEOUT_MS - elapsed) / 1000;
}

void enterExport() {
  markFirmwareHealthy();   // a deliberate reboot must not read as a failed update
  g_view = View::EXPORT;
  g_exportStartMs = millis();
  BLEDevice::deinit(true);   // hand the radio over before WiFi starts
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_SSID, AP_PASS)) {   // no access point means no export: back to monitoring
    Serial.println("[export] softAP failed");
    delay(200);
    ESP.restart();
  }
  g_http = new WebServer(80);
  g_http->on("/", []() { g_http->send_P(200, "text/html", APP_HTML); });
  g_http->on("/days", handleDays);
  g_http->onNotFound(handleFile);
  g_http->begin();
  setBacklight(BRIGHT_ALARM_LEVEL);   // the credentials have to be readable at night too
  drawExportStatic(g_screen);
  drawExportCountdown(g_screen, exportSecondsLeft(), WiFi.softAPgetStationNum());
  Serial.printf("[export] AP=%s ip=%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

// Reboot rather than tear down: restarting is the one reliable way back to a full-speed BLE radio
// after WiFi has run, for the same reason setup() reboots after its NTP sync. Clock and history
// survive.
void exitExport() {
  Serial.println("[export] resuming monitoring");
  if (g_http) g_http->stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(150);
  ESP.restart();
}

// Export mode owns the loop: no BLE, no live view, just the page until told to stop.
void serviceExport(Gesture gesture) {
  g_http->handleClient();
  if (gesture == Gesture::SWIPE_DOWN || millis() - g_exportStartMs > EXPORT_TIMEOUT_MS) {
    exitExport();   // reboots, never returns
  }
  static uint32_t lastCountdownMs = 0;
  if (millis() - lastCountdownMs > EXPORT_COUNTDOWN_REFRESH_MS) {
    lastCountdownMs = millis();
    drawExportCountdown(g_screen, exportSecondsLeft(), WiFi.softAPgetStationNum());
  }
  delay(5);   // keep the server responsive
}

// =================================================================================================
// Maintenance mode: the board on the home network, accepting a firmware upload (ArduinoOTA)
// =================================================================================================

const char* otaErrorName(ota_error_t error) {
  switch (error) {
    case OTA_AUTH_ERROR: return "wrong password";
    case OTA_BEGIN_ERROR: return "could not start";
    case OTA_CONNECT_ERROR: return "connection failed";
    case OTA_RECEIVE_ERROR: return "transfer failed";
    case OTA_END_ERROR: return "image rejected";
  }
  return "unknown error";
}

MaintenanceStatus maintenanceStatus(MaintenancePhase phase) {
  MaintenanceStatus st{};
  st.phase = phase;
  if (WiFi.status() == WL_CONNECTED) {
    strlcpy(st.network, WiFi.SSID().c_str(), sizeof(st.network));
    strlcpy(st.address, WiFi.localIP().toString().c_str(), sizeof(st.address));
  }
  st.hostname = OTA_HOSTNAME;
  return st;
}

uint32_t maintenanceSecondsLeft() {
  const uint32_t elapsed = millis() - g_maintenanceStartMs;
  return elapsed >= OTA_MODE_TIMEOUT_MS ? 0 : (OTA_MODE_TIMEOUT_MS - elapsed) / 1000;
}

// Reboot rather than tear down, for the same reason as export mode.
void exitMaintenance() {
  Serial.println("[ota] resuming monitoring");
  ArduinoOTA.end();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(150);
  ESP.restart();
}

// Same shape as enterExport(): Bluetooth is handed over to WiFi without a reboot, and leaving
// reboots. The difference is that the board joins the home network as a station instead of
// becoming an access point, so a computer on that network can push a build with espota.
void enterMaintenance() {
  markFirmwareHealthy();   // a deliberate reboot must not read as a failed update
  g_view = View::MAINTENANCE;
  g_maintenanceStartMs = millis();
  g_otaUploading = false;
  setBacklight(BRIGHT_ALARM_LEVEL);
  drawMaintenanceStatic(g_screen, maintenanceStatus(MaintenancePhase::CONNECTING));

  BLEDevice::deinit(true);   // hand the radio over before WiFi starts
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.setSleep(false);   // responsiveness over power for these ten minutes
  const bool connected = runWifiFailover([](WifiSlot slot) { return connectWifiSlot(slot); });
  if (!connected) {
    Serial.println("[ota] no network reachable");
    drawMaintenanceStatic(g_screen, maintenanceStatus(MaintenancePhase::NO_NETWORK));
    delay(4000);
    exitMaintenance();   // reboots, never returns
  }

  char password[OTA_PASSWORD_MAX];
  const bool havePassword = loadOtaPassword(password, sizeof(password));
  if (havePassword) {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPort(OTA_PORT);
    ArduinoOTA.setPassword(password);   // kept as a hash inside ArduinoOTA
    memset(password, 0, sizeof(password));
    ArduinoOTA.onStart([]() {
      g_otaUploading = true;
      Serial.println("[ota] upload started");
      drawMaintenanceProgress(g_screen, 0, 1);
    });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
      drawMaintenanceProgress(g_screen, done, total);
    });
    ArduinoOTA.onEnd([]() {   // ArduinoOTA reboots after this returns
      Serial.println("[ota] upload complete, rebooting into the new firmware");
      drawMaintenanceMessage(g_screen, "Update installed", "rebooting...");
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("[ota] failed: %s\n", otaErrorName(error));
      drawMaintenanceMessage(g_screen, "Update failed", otaErrorName(error));
      delay(4000);
      exitMaintenance();   // reboots into the firmware that was already running
    });
    ArduinoOTA.begin();
  } else {
    Serial.println("[ota] no /ota.txt on the card: upload disabled");
  }

  drawMaintenanceStatic(g_screen, maintenanceStatus(havePassword ? MaintenancePhase::READY
                                                                 : MaintenancePhase::NO_PASSWORD));
  drawMaintenanceCountdown(g_screen, maintenanceSecondsLeft());
  Serial.printf("[ota] %s host=%s ip=%s\n", havePassword ? "ready" : "disabled", OTA_HOSTNAME,
                WiFi.localIP().toString().c_str());
}

// Maintenance mode owns the loop. Once a transfer starts, handle() blocks until it ends and the
// callbacks above draw the progress, so the swipe and the timeout only apply between transfers.
void serviceMaintenance(Gesture gesture) {
  ArduinoOTA.handle();
  if (g_otaUploading) return;
  if (gesture == Gesture::SWIPE_DOWN || millis() - g_maintenanceStartMs > OTA_MODE_TIMEOUT_MS) {
    exitMaintenance();   // reboots, never returns
  }
  static uint32_t lastCountdownMs = 0;
  if (millis() - lastCountdownMs > OTA_COUNTDOWN_REFRESH_MS) {
    lastCountdownMs = millis();
    drawMaintenanceCountdown(g_screen, maintenanceSecondsLeft());
  }
  delay(5);
}

// =================================================================================================
// Views
// =================================================================================================

void showBootMessage(const char* text) {
  g_tft.fillScreen(g_screen.palette.bg);
  g_tft.setFont(&fonts::FreeSans9pt7b);
  g_tft.setTextColor(g_screen.palette.grey);
  g_tft.setTextDatum(textdatum_t::middle_center);
  g_tft.drawString(text, g_tft.width() / 2, g_tft.height() / 2);
}

void showLiveView() {
  g_view = View::LIVE;
  g_renderKeyValid = false;
  g_tft.fillScreen(g_screen.palette.bg);
}

void renderLive(const ReadingSnapshot& reading, RadioState radioState, bool stale) {
  const uint8_t alertMask = makeAlertMask(reading.effectiveHeartRate, reading.oxygenSaturation,
                                          stale, HR_LOW, HR_HIGH, SPO2_LOW);
  const LiveRenderKey key =
      makeLiveRenderKey(reading, radioState, stale, alertMask, minuteOfDay());
  if (g_renderKeyValid && key == g_lastRenderKey) return;
  char clock[8];
  clockText(clock, sizeof(clock));
  drawLive(g_screen, g_history, reading, radioState, clock, key, g_lastRenderKey,
           !g_renderKeyValid);
  g_lastRenderKey = key;
  g_renderKeyValid = true;
}

// A tap on a column of the live view opens its 24-hour chart; a tap on the chart cycles the bin width.
void handleTap(int x, int y) {
  g_lastTouchMs = millis();
  if (g_view == View::LIVE) {
    if (y < g_screen.layout.headerH) return;
    g_plotMetric = x < g_screen.layout.colW ? DayMetric::HEART_RATE : DayMetric::OXYGEN;
    g_view = View::PLOT;
    drawCurrentPlot();
  } else if (g_view == View::PLOT) {
    g_plotBinMinutes = g_plotBinMinutes == 60 ? 30 : (g_plotBinMinutes == 30 ? 15 : 60);
    drawCurrentPlot();
  }
}

// ---- alarm view ---------------------------------------------------------------------------------

AlarmRenderKey g_alarmKey;
bool g_alarmKeyValid = false;

// Entering the alarm: the screen is taken over, brightness forced up regardless of night mode, and
// the onset is on the card before anything else can go wrong.
void onAlarmStarted(const ReadingSnapshot& r) {
  g_selfTest = false;   // a real alarm during a self-test must not be labelled TEST
  g_view = View::ALARM;
  setBacklight(BRIGHT_ALARM_LEVEL);
  g_renderKeyValid = false;
  g_alarmKeyValid = false;

  const AlarmMachine* m = firingAlarm();
  const AlarmCause cause = m ? m->cause : AlarmCause::NONE;
  // The rate the firing machine acted on: the fast alarm may have heard the beat interval while the
  // screen kept the byte, and the log has to say which number raised it.
  const int acted = (m == &g_alarmHigh) ? r.highAlarmHeartRate : r.effectiveHeartRate;
  // The cause name stays the first token of the detail column: reconcileAlertLine() matches on it.
  char detail[48];
  if (acted != r.heartRate) {
    snprintf(detail, sizeof(detail), "%s band=%d beat=%dms", alarmCauseName(cause), r.heartRate,
             r.beatMs);
  } else {
    snprintf(detail, sizeof(detail), "%s", alarmCauseName(cause));
  }
  logAlert("ONSET", acted, r.oxygenSaturation, detail);
  Serial.printf("[alarm] %s hr=%d band=%d beat=%d\n", alarmCauseName(cause), acted, r.heartRate,
                r.beatMs);
}

void leaveAlarmView() {
  g_selfTest = false;
  g_view = View::LIVE;
  applyBrightness();
  applyTheme();
  g_renderKeyValid = false;
  g_alarmKeyValid = false;
  g_tft.fillScreen(g_screen.palette.bg);
}

void startSelfTest() {
  g_selfTest = true;
  g_selfTestStartMs = millis();
  setBacklight(BRIGHT_ALARM_LEVEL);
  g_alarmKeyValid = false;
  Serial.println("[alarm] self-test");
}

// The alarm owns the screen but not the radio: unlike export mode, BLE keeps running underneath, so
// the trace and the numbers stay live while the alarm is up.
void serviceAlarmView(const ReadingSnapshot& reading, RadioState radioState, bool stale) {
  const uint32_t now = millis();
  const HoldResult hold =
      holdUpdate(g_hold, g_touchDown, g_touchX, g_touchY, now, HOLD_COUNTDOWN_MS, TAP_MAX_MOVE);

  if (hold.completed) {
    if (!g_selfTest) {
      logAlert("DISMISS", reading.effectiveHeartRate, reading.oxygenSaturation, "dismissed");
      // Both, always. One hold silences the screen, so it has to snooze the other side too;
      // otherwise dismissing a slow alarm can be followed a second later by a fast one from the
      // same run of doubtful readings.
      alarmDismiss(g_alarmHigh, now);
      alarmDismiss(g_alarmLow, now);
    }
    leaveAlarmView();
    return;
  }
  if (g_selfTest && static_cast<uint32_t>(now - g_selfTestStartMs) >= SELFTEST_DURATION_S * 1000UL) {
    leaveAlarmView();
    return;
  }

  const uint32_t nowEpoch = nowEpochOrZero();
  AlarmMachine* firing = firingAlarm();
  AlarmAppearance a;
  a.machine = firing ? firing : &g_alarmHigh;   // the self-test borrows the fast machine's idle state
  a.contacts = &g_contacts;
  // The number on the alarm screen is the one the firing machine acted on.
  a.heartRate = (firing == &g_alarmHigh) ? reading.highAlarmHeartRate : reading.effectiveHeartRate;
  a.rawHeartRate = reading.heartRate;
  a.corrected = a.heartRate != reading.heartRate;
  a.oxygen = reading.oxygenSaturation;
  a.oxygenAgeMin = (g_spo2StampEpoch && nowEpoch >= g_spo2StampEpoch)
                       ? static_cast<int>((nowEpoch - g_spo2StampEpoch) / 60)
                       : -1;
  a.elapsedS = g_selfTest ? 0 : alarmElapsedS(*a.machine, nowEpoch);
  a.selfTest = g_selfTest;
  a.selfTestLeftS = static_cast<int>(SELFTEST_DURATION_S - (now - g_selfTestStartMs) / 1000);
  a.stale = stale;
  a.radioState = radioState;

  // Three independent repaints, so the fastest-changing thing does not drag the slowest through a
  // redraw: the vitals and trace when a reading lands (~20 s), the timer once a second in its own
  // corner, the frame twice a second as four thin rectangles.
  const AlarmRenderKey key{reading.sequence,   reading.effectiveHeartRate, reading.heartRate,
                           reading.highAlarmHeartRate, reading.oxygenSaturation, stale, radioState};
  if (!g_alarmKeyValid || !(key == g_alarmKey)) {
    g_alarmKey = key;
    g_alarmKeyValid = true;
    drawAlarmStatic(g_screen, g_history, a);
    drawAlarmTimer(g_screen, a);
    drawAlarmHold(g_screen, hold, g_selfTest);
    // Evidence for "the trace looks frozen": one line per repaint, so the spacing between lines is
    // the real reading cadence and `timed` says whether the trace has a time axis.
    Serial.printf("[alarm] draw hr=%d spo2=%d seq=%d hist=%d timed=%d age=%lus\n",
                  reading.effectiveHeartRate, reading.oxygenSaturation, reading.sequence,
                  g_history.count, tracePositional(g_history.epoch, g_history.count) ? 0 : 1,
                  static_cast<unsigned long>(
                      reading.lastPacketMs ? (now - reading.lastPacketMs) / 1000 : 0));
  }

  static uint32_t lastTimerS = 0xFFFFFFFF;
  const uint32_t shownS = g_selfTest ? static_cast<uint32_t>(a.selfTestLeftS) : a.elapsedS;
  if (shownS != lastTimerS) {
    lastTimerS = shownS;
    drawAlarmTimer(g_screen, a);
  }

  static bool lastFrameOn = false;
  static bool lastHoldActive = false;
  static uint8_t lastHoldPercent = 255;
  const bool frameOn = (now / ALARM_FLASH_MS) % 2 == 0;
  if (frameOn != lastFrameOn) {
    lastFrameOn = frameOn;
    drawAlarmFrame(g_screen, frameOn);
  }
  if (hold.active != lastHoldActive || hold.percent != lastHoldPercent) {
    lastHoldActive = hold.active;
    lastHoldPercent = hold.percent;
    drawAlarmHold(g_screen, hold, g_selfTest);
  }
}

// Holding the HEART cell runs the alarm self-check. This resolves while still touching, unlike a
// tap, because the countdown has to be drawn during the hold; a completed hold then suppresses the
// release so testing the alarm does not also open the 24-hour chart.
HeartHold serviceHeartHold(GestureResult& gesture) {
  // Guarded against g_selfTest: during a self-test the view is still LIVE, and without this both
  // this function and serviceAlarmView() would drive the same HoldState in one pass.
  if (g_view != View::LIVE || g_selfTest) return HeartHold::NONE;
  const Layout& L = g_screen.layout;
  const bool onHeart = g_touchDown && g_touchY >= L.headerH && g_touchX < L.colW;
  const HoldResult h = holdUpdate(g_hold, onHeart, g_touchX, g_touchY, millis(),
                                  HOLD_COUNTDOWN_MS, TAP_MAX_MOVE);
  if (h.completed) {
    startSelfTest();
    return HeartHold::COMPLETED;
  }
  static bool wasHolding = false;
  if (h.active) {
    // The same countdown the alarm screen shows, over the heart cell.
    char seconds[8];
    snprintf(seconds, sizeof(seconds), "%u", static_cast<unsigned>(h.secondsLeft));
    g_tft.setFont(&fonts::FreeSansBold9pt7b);
    g_tft.setTextColor(PANEL_RED, g_screen.palette.bg);   // opaque, so 3 -> 2 -> 1 overwrites cleanly
    g_tft.setTextDatum(textdatum_t::middle_center);
    g_tft.drawString(seconds, L.colW / 2, L.headerH + L.rowH / 2);
    wasHolding = true;
    return HeartHold::COUNTING;
  }
  if (wasHolding) {   // abandoned: repaint the live view over the digit
    wasHolding = false;
    g_renderKeyValid = false;
  }
  if (gesture.gesture == Gesture::TAP && holdConsumedTap(g_hold)) gesture.gesture = Gesture::NONE;
  return HeartHold::NONE;
}

// =================================================================================================
// Alarms
// =================================================================================================

// Driven every pass before rendering, so the alarm can take the screen in the same pass it fires.
void serviceAlarms(const ReadingSnapshot& reading, bool stale) {
  const uint32_t now = millis();
  static int lastSequence = -1;
  if (reading.sequence != lastSequence && readingIsFresh(reading, now)) {
    lastSequence = reading.sequence;
    const uint32_t nowEpoch = nowEpochOrZero();
    if (reading.oxygenSaturation > 0) g_spo2StampEpoch = nowEpoch;
    // The slow alarm hears the displayed rate. The fast alarm hears the faster decode whenever the
    // band contradicts itself (band_protocol.h): a doubled interval must never pose as bradycardia,
    // and a tachycardia whose byte arrives halved must still be counted.
    const bool highCorrected = reading.highAlarmHeartRate != reading.heartRate;
    const AlarmEvent high = alarmOnReading(g_alarmHigh, ALARM_HIGH_CFG, reading.highAlarmHeartRate,
                                           now, nowEpoch, highCorrected);
    const AlarmEvent low = alarmOnReading(g_alarmLow, ALARM_LOW_CFG, reading.effectiveHeartRate,
                                          now, nowEpoch, reading.heartRateCorrected);
    if (high.alarmStarted || low.alarmStarted) onAlarmStarted(reading);
    if (high.episodeResolved) {
      logAlert("RESOLVED", reading.highAlarmHeartRate, reading.oxygenSaturation, "");
    }
    if (low.episodeResolved) {
      logAlert("RESOLVED", reading.effectiveHeartRate, reading.oxygenSaturation, "");
    }
  }
  const AlarmEvent high = alarmTick(g_alarmHigh, ALARM_HIGH_CFG, now, stale);
  const AlarmEvent low = alarmTick(g_alarmLow, ALARM_LOW_CFG, now, stale);
  if (high.alarmStarted || low.alarmStarted) onAlarmStarted(reading);
}

// Each new measurement goes to the daily log, the 24-hour bins and the one-hour history.
void recordNewReading(const ReadingSnapshot& reading) {
  static int lastLogged = -1;
  if (reading.sequence == lastLogged || !readingIsFresh(reading, millis())) return;
  lastLogged = reading.sequence;
  logVitalsRow(reading);
  const int minute = minuteOfDay();
  if (minute >= 0) {
    dayBinsAdd(g_dayBins, reading.effectiveHeartRate, reading.oxygenSaturation, minute);
  }
  historyPush(g_history, reading.effectiveHeartRate, reading.oxygenSaturation, nowEpochOrZero());
}

// Skipped while the alarm is up: the alarm forces full brightness regardless of the hour, and
// letting the schedule dim it back down would be the worst bug this feature could have.
void serviceBacklight() {
  static uint32_t lastMs = 0;
  if (g_view == View::ALARM || g_selfTest) return;
  if (millis() - lastMs <= BACKLIGHT_REFRESH_MS) return;
  lastMs = millis();
  applyBrightness();
  applyTheme();
}

// A freshly installed firmware boots on probation (firmware_health.h). It is confirmed once the
// scan is running and the loop has survived its first minute; a crash before then rolls back.
void serviceFirmwareHealth() {
  static bool done = false;
  if (done) return;
  if (millis() < OTA_HEALTHY_AFTER_MS) return;
  if (!readRadioRuntime().scanStarted) return;
  markFirmwareHealthy();
  done = true;
}

// =================================================================================================
// setup / loop
// =================================================================================================

void setup() {
  Serial.begin(115200);
  delay(300);
  // Latch the true reset cause before the NTP soft reboot can overwrite it with ESP_RST_SW.
  {
    const esp_reset_reason_t reason = esp_reset_reason();
    if (reason != ESP_RST_SW) g_originalResetReason = reason;
  }

  initTouchPins();
  g_tft.init();
  for (int r = 0; r < 4; ++r) {   // clear the whole panel memory, whichever way it is mapped
    g_tft.setRotation(r);
    g_tft.fillScreen(g_screen.palette.bg);
  }
  g_tft.setRotation(DISPLAY_ROTATION);
  g_tft.setBrightness(BRIGHT_DAY_LEVEL);   // daytime level until the clock is known
  g_screen.layout = layoutFor(g_tft.width(), g_tft.height());
  setenv("TZ", TZ_INFO, 1);   // re-applied each boot: the environment does not survive a reboot
  tzset();

  showBootMessage("SD card...");
  initSd();

  // WiFi coexistence throttles BLE badly and a deinit cannot fully undo it. So a cold boot syncs NTP
  // once and soft-reboots into BLE-only mode; the RTC clock survives, so the second boot skips WiFi
  // and BLE runs at full speed.
  struct tm now{};
  if (!readLocalClock(now) && !g_syncedThisPower) {
    showBootMessage("WiFi clock sync (one-time)...");
    if (syncTimeOverWifi() && readLocalClock(now)) {
      g_syncedThisPower = true;
      showBootMessage("clock set - rebooting for BLE...");
      delay(250);
      ESP.restart();
    }
  }
  g_timeReady = readLocalClock(now);
  logBoot();
  applyBrightness();
  applyTheme();

  showBootMessage("Loading history...");
  loadTodayFromCsv();
  loadContacts();
  reconcileOpenEpisode();
  if (firingAlarm() != nullptr) {
    g_view = View::ALARM;
    setBacklight(BRIGHT_ALARM_LEVEL);
  }

  showBootMessage("Bluetooth...");
  BLEDevice::init("");
  g_bleScanner = BLEDevice::getScan();
  g_bleScanner->setAdvertisedDeviceCallbacks(new BandScanCallbacks(), true);
  startBleScan(millis());
  g_tft.fillScreen(g_screen.palette.bg);
  Serial.printf("[ready] sd=%d time=%d\n", g_sdReady, g_timeReady);
}

void loop() {
  const uint32_t now = millis();

  // One touch sample per pass, taken even while the alarm owns the screen so the hold countdown
  // stays fresh. Gesture actions are suppressed while it does; see the view guards below.
  g_touchDown = readTouch(g_touchX, g_touchY);
  GestureResult gesture = gestureUpdate(g_gesture, g_touchDown, g_touchX, g_touchY, now,
                                        SWIPE_MIN_DY, TAP_MAX_MOVE, TAP_MAX_MS);

  if (g_view == View::EXPORT) {
    serviceExport(gesture.gesture);
    return;
  }
  if (g_view == View::MAINTENANCE) {
    serviceMaintenance(gesture.gesture);
    return;
  }

  // Swipe up shares the logged data; swipe down accepts a firmware update. Neither during an alarm
  // or a self-test: both hand Bluetooth over to WiFi and replace the screen, which would hide the
  // alarm and stop the readings feeding it.
  const bool canLeaveLive = g_view == View::LIVE && !g_selfTest && firingAlarm() == nullptr;
  if (gesture.gesture == Gesture::SWIPE_UP && canLeaveLive) {
    enterExport();
    return;
  }
  if (gesture.gesture == Gesture::SWIPE_DOWN && canLeaveLive) {
    enterMaintenance();
    return;
  }

  const HeartHold heartHold = serviceHeartHold(gesture);
  if (heartHold == HeartHold::COMPLETED) return;

  const bool alarmOwned = g_view == View::ALARM || g_selfTest;
  if (gesture.gesture == Gesture::TAP && !alarmOwned) handleTap(gesture.tapX, gesture.tapY);
  if (g_view == View::PLOT && now - g_lastTouchMs > PLOT_AUTO_RETURN_MS) showLiveView();

  const RadioState radioState = serviceRadioRecovery(now);
  const ReadingSnapshot reading = readSnapshot();
  const bool stale = readingIsStale(reading, millis());

  serviceAlarms(reading, stale);

  if (g_view == View::ALARM || g_selfTest) {
    serviceAlarmView(reading, radioState, stale);   // logging below keeps filling behind the alarm
  } else if (g_view == View::LIVE && heartHold == HeartHold::NONE) {
    renderLive(reading, radioState, stale);   // skipped mid-hold, or it would erase the digit
  }

  serviceBacklight();
  recordNewReading(reading);
  serviceFirmwareHealth();

  delay(g_gesture.touching ? LOOP_DELAY_TOUCHING_MS : LOOP_DELAY_IDLE_MS);
}
