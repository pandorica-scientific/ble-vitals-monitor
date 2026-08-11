// cyd_vitals.ino — Baby Sensor Relax monitor + logger (CYD ESP32-2432S028).
// RECEIVE-ONLY passive BLE. Never connects/writes/pairs -> base link untouched.
//
// Display: big HR + SpO2, skin temp small by the name, 1-hour BPM/SpO2 sparklines with
//   yellow/red warning lines. Tap a column -> 24h avg±std chart (tap cycles 1h/30m/15m;
//   auto-returns to live after 10 s of no touch).
// Also: WiFi+NTP clock (Europe/Warsaw) synced once at boot then reboots BLE-only (coexistence),
//   per-reading CSV logging to microSD (one file per day), and a backlight that drops to its
//   lowest step between 20:00 and 08:00 so it doesn't light up the room at night.
// Power: use a stable regulated 5 V supply. The BLE scan is duty-cycled and the daytime
//   backlight runs below full to avoid unnecessary work and heat (see SCAN_* and BRIGHT_DAY).
// Export: swipe UP on the live view and the board becomes its own WiFi access point serving the
//   logged CSVs to a phone, so a week or a month can be pulled anywhere without removing the SD
//   card. Swipe DOWN (or wait out the timeout) to resume. Monitoring is PAUSED while it is up.
//
// Panel: TPM408 = ILI9342 320x240. Touch: XPT2046 bit-banged (own pins, no SD SPI clash).
// Decode: HR = byte10, SpO2 = byte13, SKIN C = big-endian uint16(bytes 6-7)/10.
// Thresholds are ARBITRARY, NOT medical advice. Hobby device, not a safety monitor.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_system.h>    // esp_reset_reason() - see logBoot()
#include <WebServer.h>
#include <time.h>
#include <math.h>
#include "band_protocol.h"
#include "live_render_key.h"
#include "radio_health.h"
#include "wifi_failover.h"
#include "critical_alarm.h"
#include "trace_window.h"
#include "hold_gesture.h"
#include "alert_log.h"
#include "contacts.h"
#include "app_html.h"      // the report page served in export mode (PROGMEM)

#define ROTATION 0
#define HR_LOW 90
#define HR_HIGH 180
#define SPO2_LOW 90
#define STALE_MS 30000
// ---- critical tachycardia alarm ----
// Sized against a ten-minute decision window: a sustained rate at or above HR_CRIT means a
// hospital visit within roughly ten minutes, so a one-minute confirmation costs 10% of it.
// HR_CANCEL deliberately equals HR_HIGH so the three levels read as one coherent scale.
#define HR_CRIT             200   // bpm at or above which the alarm arms
#define HR_SUSTAIN          190   // the last reading of the window must be at or above this
#define HR_CANCEL           180   // a reading below this abandons the window
#define HR_COLLAPSE_FROM    170   // previous reading at or above this...
#define HR_COLLAPSE_TO       60   // ...and this one at or below it escalates at once
#define HR_RAIL             255   // the uint8_t ceiling; shown as ">=255"
#define CRIT_MIN_HIGH         2   // readings at or above HR_CRIT needed in one window
#define CRIT_CONFIRM_MS   60000   // confirmation window length
#define ALARM_SNOOZE_MS  600000   // suppression after a dismissal, so the board can be carried
#define ALARM_FLASH_MS      500   // half-period of the perimeter flash
#define HOLD_COUNTDOWN_MS  3000   // hold duration for dismissal and self-test alike
// 60 s, not 10: the heart rate updates roughly every 20 seconds, so a ten-second test never shows
// the trace move and cannot demonstrate that it is live. A minute spans about three readings.
// A three-second hold ends it early, so it never traps the screen.
#define SELFTEST_DURATION_S  60   // how long the test alarm runs before returning by itself
#define SPO2_STALE_MIN       20   // minutes after which the oxygen reading is greyed
// backlight: dim overnight so the display doesn't light up the room
#define NIGHT_START_MIN (20*60)   // 20:00 -> dim
#define NIGHT_END_MIN   (8*60)    // 08:00 -> full
#define FORCE_NIGHT 0             // test aid: set to 1 to force night mode regardless of the clock,
                                  // so the dark theme can be checked without waiting for 20:00
#define BRIGHT_DAY   140          // ~55%: backlight current tracks PWM duty closely, so this is
                                  // about half the power of 255 and still easily readable indoors
#define BRIGHT_NIGHT 1            // lowest non-zero PWM step (0 would switch the backlight off)
// BLE scan duty cycle. Receiving costs ~90-100 mA, so the old 99%-duty scan (99/100) dominated the
// power budget for nothing: the wristband advertises every ~1.5 s and only produces a new heart
// rate every ~20 s (docs/PROTOCOL.md).
// MEASURED on this board with the display and SD logging running - reception falls off much faster
// than the duty ratio suggests, because rendering and SD writes compete with the radio:
//   300/1000 (30%) -> worst gap 13.0 s, ~15 adverts/30 s  - too close to STALE_MS, drops to "--"
//   500/1000 (50%) -> worst gap  2.0 s, ~29 adverts/30 s  - what we ship
// Re-measure the worst gap against STALE_MS before lowering the window.
#define SCAN_INTERVAL_MS 1000     // how often a scan window starts
#define SCAN_WINDOW_MS   500      // how long the radio listens inside that window
// ---- data export mode: swipe UP from the live view, swipe DOWN to resume ----
// The board becomes its own WiFi access point and serves the logged CSVs to a phone, so the data
// can be pulled anywhere (a doctor's office) with no home network, no hotspot and no SD removal.
// BLE cannot run while WiFi does (see the coexistence note in setup()), so this mode PAUSES
// monitoring - hence the loud on-screen warning and the automatic return below.
#define AP_SSID    "BabyVitals"
#define AP_PASS    "babyvitals"   // WPA2 needs >=8 chars. Shown on screen, so nothing to memorise.
#define EXPORT_TIMEOUT_MS 600000  // resume monitoring by itself after 10 min, in case of a stray swipe
// Gesture thresholds. The swipe must cross more than half the 240 px height and be clearly more
// vertical than horizontal, so brushing the screen while moving the board cannot stop monitoring.
#define SWIPE_MIN_DY  130         // px of vertical travel before it counts as a swipe
#define TAP_MAX_MOVE  20          // px; anything that moves further than this is not a tap
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23
#define SD_CS 5
// XPT2046 touch (bit-banged)
#define T_CLK 25
#define T_MOSI 32
#define T_MISO 39
#define T_CS 33
#define T_IRQ 36
// touch calibration (from 5-point cal). TOUCH_ prefix on purpose: bare TX1/RX1 etc. are already
// UART pin macros in the ESP32 core, and redefining them here shadowed the core's values.
#define TOUCH_X0 170
#define TOUCH_X1 3840
#define TOUCH_Y0 320
#define TOUCH_Y1 3760

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9342 _panel; lgfx::Bus_SPI _bus; lgfx::Light_PWM _light;
public:
  LGFX() {
    { auto c=_bus.config(); c.spi_host=HSPI_HOST; c.spi_mode=0; c.freq_write=40000000; c.freq_read=16000000;
      c.spi_3wire=false; c.use_lock=true; c.dma_channel=SPI_DMA_CH_AUTO;
      c.pin_sclk=14; c.pin_mosi=13; c.pin_miso=12; c.pin_dc=2; _bus.config(c); _panel.setBus(&_bus); }
    { auto c=_panel.config(); c.pin_cs=15; c.pin_rst=-1; c.pin_busy=-1;
      c.panel_width=320; c.panel_height=240; c.offset_x=0; c.offset_y=0; c.offset_rotation=0;
      c.readable=true; c.invert=false; c.rgb_order=true; c.dlen_16bit=false; c.bus_shared=true; _panel.config(c); }  // BGR panel: fixes red/blue swap
    { auto c=_light.config(); c.pin_bl=21; c.invert=false; c.freq=44100; c.pwm_channel=7; _light.config(c); _panel.setLight(&_light); }
    setPanel(&_panel);
  }
};
LGFX tft;
SPIClass sdSPI(VSPI);

// ---------- shared vitals ----------
ReadingSnapshot g_reading;
portMUX_TYPE g_readingMux = portMUX_INITIALIZER_UNLOCKED;

ReadingSnapshot readSnapshot() {
  portENTER_CRITICAL(&g_readingMux);
  ReadingSnapshot copy = g_reading;
  portEXIT_CRITICAL(&g_readingMux);
  return copy;
}

void publishReading(const BandReading& frame, int rssi, uint32_t nowMs) {
  portENTER_CRITICAL(&g_readingMux);
  g_reading = mergeBandReading(g_reading, frame, rssi, nowMs);
  portEXIT_CRITICAL(&g_readingMux);
}

void holdSkinTemperature(float skinC) {
  if (skinC < BAND_SKIN_MIN_C || skinC > BAND_SKIN_MAX_C) return;
  portENTER_CRITICAL(&g_readingMux);
  g_reading.skinC = skinC;
  g_reading.skinValid = true;
  portEXIT_CRITICAL(&g_readingMux);
}

// ---------- protected Bluetooth health telemetry ----------
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

RadioRuntime g_radio;
portMUX_TYPE g_radioMux = portMUX_INITIALIZER_UNLOCKED;
BLEScan* g_bleScanner = nullptr;

RadioRuntime readRadioRuntime();
RadioState classifyRadio(uint32_t nowMs, const RadioRuntime& radio);

RadioRuntime readRadioRuntime() {
  portENTER_CRITICAL(&g_radioMux);
  RadioRuntime copy = g_radio;
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
  g_bleScanner->setActiveScan(false);
  g_bleScanner->setInterval(SCAN_INTERVAL_MS);
  g_bleScanner->setWindow(SCAN_WINDOW_MS);
  bool started = g_bleScanner->start(0, nullptr, false);
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

const char* radioStateLabel(RadioState state) {
  switch (state) {
    case RadioState::STARTING: return "STARTING";
    case RadioState::RECEIVING: return "RECEIVING";
    case RadioState::BAND_MISSING: return "BAND_MISSING";
    case RadioState::SCANNER_SILENT: return "SCANNER_SILENT";
  }
  return "UNKNOWN";
}

RadioState serviceRadioRecovery(uint32_t nowMs) {
  RadioRuntime radio = readRadioRuntime();
  RadioState state = classifyRadio(nowMs, radio);
  static bool havePrevious = false;
  static RadioState previous = RadioState::STARTING;

  if (!havePrevious || state != previous) {
    Serial.printf("[ble] state=%s total=%lu band=%lu restarts=%lu\n",
                  radioStateLabel(state),
                  static_cast<unsigned long>(radio.totalAdvertisements),
                  static_cast<unsigned long>(radio.bandAdvertisements),
                  static_cast<unsigned long>(radio.restartCount));
    previous = state;
    havePrevious = true;
  }

  if (!shouldRestartScan(state, radio.restartAttempted, nowMs,
                         radio.lastRestartAttemptMs)) {
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

bool g_timeReady=false, g_sdReady=false;
RTC_DATA_ATTR bool g_syncedThisPower=false;   // survives the soft reboot below
// The real reason this power cycle started, carried across the deliberate NTP soft-reboot below.
// Without this every restart would be recorded as SW: the cold boot has no clock yet, so it cannot
// write a timestamped line, and by the time boot 2 can, esp_reset_reason() only reports our own
// ESP.restart(). RTC memory survives a SW reset but not a power cut, which is exactly the
// distinction logBoot() needs.
RTC_DATA_ATTR esp_reset_reason_t g_origReason=ESP_RST_UNKNOWN;
// The alarm latch must outlive a watchdog reset, a panic and the NTP soft reboot, so it lives in
// RTC memory alongside g_origReason. RTC memory does NOT survive a power cut, which is why the
// onset is also written to /alerts.csv the moment it happens and reconciled at boot.
RTC_DATA_ATTR AlarmMachine g_alarm;
const AlarmConfig ALARM_CFG{HR_CRIT,HR_SUSTAIN,HR_CANCEL,HR_COLLAPSE_FROM,HR_COLLAPSE_TO,
                            CRIT_MIN_HIGH,CRIT_CONFIRM_MS,ALARM_SNOOZE_MS};
Contacts g_contacts;
HoldState g_hold;
int g_alarmKey=0; bool g_alarmKeyValid=false;
bool g_selfTest=false; uint32_t g_selfTestStart=0;
uint32_t g_spo2StampEpoch=0;
// ---------- rolling ~1-hour history for the mini sparklines (~180 readings @ ~20s) ----------
// histEpoch gives the trace a real time axis. Without it a four-minute dropout draws as a
// straight line between two adjacent samples, which reads as a steady rate for four minutes -
// the opposite of what happened. 720 bytes buys the difference between "we don't know" and a
// confident lie, and the alarm trace is read precisely when dropouts are most likely.
#define HN 180
uint8_t hrHist[HN], spHist[HN]; uint32_t histEpoch[HN]; int histHead=0, histCnt=0;
void pushHist(int hr,int sp,uint32_t epoch){
  hrHist[histHead]=constrain(hr,0,254); spHist[histHead]=constrain(sp,0,254);
  histEpoch[histHead]=epoch;
  histHead=(histHead+1)%HN; if(histCnt<HN) histCnt++;
}

class CB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    uint32_t nowMs = millis();
    noteAnyAdvertisement(nowMs);
    if (!dev.haveManufacturerData()) return;
    String manufacturer = dev.getManufacturerData();
    BandReading decoded{};
    if (!decodeBandFrame(
            reinterpret_cast<const uint8_t*>(manufacturer.c_str()),
            manufacturer.length(), decoded)) {
      return;
    }
    noteBandAdvertisement(nowMs);
    publishReading(decoded, dev.getRSSI(), nowMs);
  }
};

// ---------- 24h bins (96 x 15-min) ----------
struct Bin { uint16_t nH, nO; float sH, sqH, sO, sqO; };
Bin bins[96];
void addReading(int hr, int spo2, int minOfDay) {
  int b = minOfDay/15; if (b<0||b>=96) return;
  if (hr>0)  { bins[b].nH++; bins[b].sH+=hr;  bins[b].sqH+=(float)hr*hr; }
  if (spo2>0){ bins[b].nO++; bins[b].sO+=spo2;bins[b].sqO+=(float)spo2*spo2; }
}

// ---------- SD + WiFi + NTP ----------
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t NTP_TIMEOUT_MS = 15000;
constexpr char TZ_INFO[] = "CET-1CEST,M3.5.0,M10.5.0/3";
constexpr char NTP_SERVER_1[] = "pool.ntp.org";
constexpr char NTP_SERVER_2[] = "time.nist.gov";

struct WifiCredential {
  String ssid;
  String password;
};

bool loadWifiCredential(WifiSlot slot, WifiCredential& out);
bool tryWifiSlot(WifiSlot slot);

bool initSD() {
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  g_sdReady = SD.begin(SD_CS, sdSPI) && SD.cardType()!=CARD_NONE;
  Serial.println(g_sdReady?"[SD] OK":"[SD] FAILED"); return g_sdReady;
}

bool readLocalClock(struct tm& out) {
  time_t now;
  time(&now);
  localtime_r(&now, &out);
  return out.tm_year > 120;
}

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

bool tryWifiSlot(WifiSlot slot) {
  WifiCredential credential;
  if (!loadWifiCredential(slot, credential)) {
    credential.password = "";
    Serial.printf("[wifi] %s unavailable\n", wifiSlotLabel(slot));
    return false;
  }

  Serial.printf("[wifi] trying %s\n", wifiSlotLabel(slot));
  WiFi.begin(credential.ssid.c_str(), credential.password.c_str());
  uint32_t startedMs = millis();
  while (WiFi.status() != WL_CONNECTED &&
         static_cast<uint32_t>(millis() - startedMs) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }

  bool synced = false;
  if (WiFi.status() == WL_CONNECTED) {
    configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
    startedMs = millis();
    struct tm localTime{};
    while (static_cast<uint32_t>(millis() - startedMs) < NTP_TIMEOUT_MS) {
      if (getLocalTime(&localTime, 200) && localTime.tm_year > 120) {
        synced = true;
        break;
      }
      delay(50);
    }
  }

  Serial.printf("[wifi] %s %s\n", wifiSlotLabel(slot),
                synced ? "time-synced" : "failed");
  credential.password = "";
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
  bool synced = runWifiFailover(
      [](WifiSlot slot) { return tryWifiSlot(slot); });
  shutdownWifi();
  return synced;
}
// One definition of the log filename: the writer (logRow) and the reader (loadCsvToday) must never
// disagree about which file "today" is.
void csvName(char* out,const struct tm* tm){ strftime(out,32,"/vitals_%Y-%m-%d.csv",tm); }
void csvName(char* out){ struct tm tm{}; if(readLocalClock(tm)) csvName(out,&tm); else out[0]='\0'; }
// 0 means "no usable timestamp" - the trace falls back to a positional axis and says so, rather
// than placing samples at epoch zero and drawing a confident wrong time axis.
uint32_t nowEpochOrZero(){ struct tm tm{}; if(!readLocalClock(tm)) return 0; return (uint32_t)mktime(&tm); }
// Rows in today's CSV carry a full timestamp; combine it with today's date so reloaded history
// lands on the same axis as live readings. Seconds matter: readings arrive about every 22 s, so
// quantising to the minute would stack two or three of them on one x-position and turn a real
// dropout into an apparent one.
uint32_t csvRowEpoch(int hh,int mm,int ss){
  struct tm tm{}; if(!readLocalClock(tm)) return 0;
  tm.tm_hour=hh; tm.tm_min=mm; tm.tm_sec=ss;
  return (uint32_t)mktime(&tm);
}
void logRow(int hr,int spo2,float skin,bool sv){
  if(!g_sdReady||!g_timeReady) return; struct tm tm{}; if(!readLocalClock(tm)) return;
  char fn[32]; csvName(fn,&tm); bool isNew=!SD.exists(fn);
  File f=SD.open(fn,FILE_APPEND); if(!f) return;
  if(isNew) f.println("timestamp,hr_bpm,spo2_pct,skin_c");
  char ts[24]; strftime(ts,24,"%F %T",&tm);
  if(sv) f.printf("%s,%d,%d,%.1f\n",ts,hr,spo2,skin); else f.printf("%s,%d,%d,\n",ts,hr,spo2);
  f.close();
}
// /alerts.csv is append-only. Rewriting a row in place on an SD card is not crash-safe; appending
// events is, so a brownout mid-episode still leaves a readable file and an open episode.
void logAlert(const char* event,int hr,int spo2,const char* detail){
  if(!g_sdReady) return; struct tm tm{}; if(!readLocalClock(tm)) return;
  char ts[24]; strftime(ts,sizeof(ts),"%F %T",&tm);
  bool isNew=!SD.exists("/alerts.csv");
  File f=SD.open("/alerts.csv",FILE_APPEND); if(!f) return;
  if(isNew) f.println("timestamp,event,hr_bpm,spo2_pct,detail");
  char row[128]; formatAlertRow(row,sizeof(row),ts,event,hr,spo2,detail);
  f.println(row); f.close();
}

// The repository ships no /contacts.txt and contains no real numbers: this project is published
// for other people to build, and compiling one family's hospital numbers into it would put those
// numbers on a stranger's screen during their emergency.
void loadContacts(){
  g_contacts=Contacts{};
  if(!g_sdReady) return;
  File f=SD.open("/contacts.txt",FILE_READ); if(!f) return;
  char buf[128]; size_t n=f.readBytes(buf,sizeof(buf)-1); buf[n]='\0'; f.close();
  parseContacts(buf,g_contacts);
}

// An ONSET with no later DISMISS means the board died mid-episode. Coming back up silent would be
// the worst possible behaviour, so the alarm is re-entered.
void reconcileOpenEpisode(){
  if(!g_sdReady||!g_timeReady) return;
  File f=SD.open("/alerts.csv",FILE_READ); if(!f) return;
  OpenEpisode open{};
  while(f.available()){
    String ln=f.readStringUntil('\n'); if(ln.length()<20) continue;
    struct tm tm{}; if(strptime(ln.c_str(),"%Y-%m-%d %H:%M:%S",&tm)==nullptr) continue;
    reconcileAlertLine(ln.c_str(),(uint32_t)mktime(&tm),open);
  }
  f.close();
  if(open.open && g_alarm.state!=AlarmState::ALARM){
    g_alarm.state=AlarmState::ALARM; g_alarm.cause=AlarmCause::CONFIRMED_HIGH;
    g_alarm.onsetEpoch=open.onsetEpoch; g_alarm.resolved=false;
    logAlert("BOOT_RESUME",0,0,"recovered from card");
  }
}

void loadCsvToday(){
  if(!g_sdReady||!g_timeReady) return; char fn[32]; csvName(fn);
  File f=SD.open(fn); if(!f){Serial.println("[csv] no file to load");return;}
  f.readStringUntil('\n'); int n=0;
  while(f.available()){
    String ln=f.readStringUntil('\n'); if(ln.length()<19) continue;
    int hh=ln.substring(11,13).toInt(), mm=ln.substring(14,16).toInt(), ss=ln.substring(17,19).toInt();
    int c1=ln.indexOf(','); int c2=ln.indexOf(',',c1+1); int c3=ln.indexOf(',',c2+1);
    if(c1<0||c2<0) continue;
    int hr=ln.substring(c1+1,c2).toInt();
    int sp=ln.substring(c2+1,c3<0?ln.length():c3).toInt();
    addReading(hr,sp,hh*60+mm); pushHist(hr,sp,csvRowEpoch(hh,mm,ss)); n++;   // last HN kept -> ~1h sparkline survives reboot
    // remember the last valid skin temp so the display holds it (like the app) instead of "--"
    if(c3>=0){ String sf=ln.substring(c3+1); sf.trim();
      if(sf.length()){ float sk=sf.toFloat(); if(sk>=BAND_SKIN_MIN_C&&sk<=BAND_SKIN_MAX_C){ holdSkinTemperature(sk); } } }
  }
  f.close();
  ReadingSnapshot held = readSnapshot();
  Serial.printf("[csv] loaded %d rows into bins (heldSkin=%.1f valid=%d)\n",n,held.skinC,held.skinValid);
}
// Why the board restarted, appended to its own file so the CSV format the report app parses is
// untouched. There is no current meter on this build, so this is the only evidence that separates
// the three candidate causes of a dead screen in the morning:
//   POWERON  - cold boot or supply interruption    -> confirms neither the source nor the cause
//   BROWNOUT - the ESP32 supply fell too low       -> check the 5 V source, cable and regulator
//   PANIC/WDT/TASK_WDT - firmware crashed          -> investigate the software path
// A single POWERON when you plug it in is normal. Extra entries overnight need investigation.
void logBoot(){
  if(!g_sdReady || !g_timeReady) return;
  const char* r; switch(g_origReason){
    case ESP_RST_POWERON:  r="POWERON";  break;   case ESP_RST_BROWNOUT: r="BROWNOUT"; break;
    case ESP_RST_SW:       r="SW";       break;   case ESP_RST_PANIC:    r="PANIC";    break;
    case ESP_RST_INT_WDT:  r="INT_WDT";  break;   case ESP_RST_TASK_WDT: r="TASK_WDT"; break;
    case ESP_RST_WDT:      r="WDT";      break;   case ESP_RST_DEEPSLEEP:r="DEEPSLEEP";break;
    case ESP_RST_EXT:      r="EXT";      break;   // EN pin pulled low - i.e. a flash/serial reset
    default:               r="OTHER";    break; }
  // The numeric code goes in too: a bare "OTHER" at 03:00 is a dead end, and the enum has more
  // members (USB, JTAG, CPU_LOCKUP, PWR_GLITCH) than are worth spelling out here.
  File f=SD.open("/boot.log",FILE_APPEND); if(!f) return;
  struct tm tm{}; char ts[24]="?";
  if(readLocalClock(tm)) strftime(ts,24,"%F %T",&tm);
  f.printf("%s,%s,%d\n",ts,r,(int)g_origReason); f.close();
  Serial.printf("[boot] %s reset=%s(%d)\n",ts,r,(int)g_origReason);
}
bool nowHM(char* o){ struct tm tm{}; if(!readLocalClock(tm))return false; strftime(o,8,"%H:%M",&tm); return true; }
int minuteOfDay(){ struct tm tm{}; if(!readLocalClock(tm))return -1; return tm.tm_hour*60+tm.tm_min; }
// 20:00 -> 08:00 is "night": dim backlight + dark theme.
// Gated on g_timeReady: without a valid clock the day look is the safe default.
bool isNight(){
  if(FORCE_NIGHT) return true;
  if(!g_timeReady) return false;
  int m=minuteOfDay();
  return m>=0 && (m>=NIGHT_START_MIN || m<NIGHT_END_MIN);
}
int wantBrightness(){ return isNight()?BRIGHT_NIGHT:BRIGHT_DAY; }
void setBacklight(int want){                     // single owner of the PWM; only writes on a change
  static int cur=-1; if(want!=cur){ tft.setBrightness(want); cur=want; }
}
void applyBrightness(){ setBacklight(wantBrightness()); }

// ---------- UI ----------
// This panel drives its pixels inverted: the code writes TFT_BLACK and the screen shows white.
// The daytime look below is therefore "white background, dark text" as seen on the device, and it
// is left exactly as it was. At night every background/chrome colour is replaced by its 16-bit
// complement, which flips the panel to a black background with light text. The VALUE colours
// (heart, oxygen, skin, alerts) are deliberately NOT flipped so the numbers keep their usual hues.
#define INV(c) ((uint16_t)~(uint16_t)(c))
const uint16_t GREY_D=0x9CD3, DIM_D=0x52AA, LINE_D=0x2965, GRID_D=0x1082, BG_D=TFT_BLACK;
uint16_t GREY=GREY_D, DIM=DIM_D, LINE=LINE_D, GRID=GRID_D, BG=BG_D;
int W,H,HDR,COLW,RH;
enum View { LIVE, PLOT, EXPORT, ALARM };
View view=LIVE; int plotMetric=0; int plotBinMin=60;   // 60/30/15

// Included here, not with the other headers at the top: it draws, so it needs tft, W, H, DIM and
// the history buffer to already exist. Kept out of this file so the sketch does not absorb two
// hundred lines of layout code.
#include "alarm_render.h"
uint32_t g_exportStart=0;                              // millis() when export mode began
LiveRenderKey g_lastRenderKey;
bool g_renderKeyValid = false;

void cell(int col,int r,const char* label,const char* val,const char* unit,uint16_t color){
  int x=col*COLW,y=HDR+r*RH;
  tft.fillRect(x,y,COLW,RH,BG);        // clear our own area, so the live view never full-screen wipes
  tft.setTextDatum(textdatum_t::top_left); tft.setFont(&fonts::FreeSans9pt7b); tft.setTextColor(GREY);
  tft.drawString(label,x+8,y+2);
  tft.setTextDatum(textdatum_t::bottom_right); tft.drawString(unit,x+COLW-8,y+RH-4);
  tft.setFont(&fonts::Font7); tft.setTextSize(1); tft.setTextColor(color);
  tft.setTextDatum(textdatum_t::top_left); tft.drawString(val,x+8,y+20);
}
void drawHeader(const ReadingSnapshot& reading,RadioState radioState,bool stale){
  tft.fillRect(0,2,W,HDR-4,BG);
  tft.setFont(&fonts::FreeSansBold9pt7b); tft.setTextDatum(textdatum_t::top_left);
  tft.setTextColor(GREY); tft.drawString("Oliwia",6,4);
  if(reading.skinValid && !stale){ char sb[10]; snprintf(sb,10,"%.1fC",reading.skinC);   // skin temp small, by the name
    tft.setFont(&fonts::FreeSans9pt7b); tft.setTextColor(0xFE79); tft.setTextDatum(textdatum_t::top_left);
    tft.drawString(sb,74,5); }
  char hm[8]; bool haveTime=nowHM(hm);
  tft.setFont(&fonts::FreeSans9pt7b); tft.setTextDatum(textdatum_t::top_right); char buf[40];
  if(radioState == RadioState::RECEIVING){
    tft.setTextColor(GREY);
    snprintf(buf,sizeof(buf),"%s%s%s%d",
             haveTime?hm:"",haveTime?"  ":"","sig",reading.signal);
  } else {
    tft.setTextColor(0xEB44);
    const char* status = radioState == RadioState::STARTING
                             ? "scan"
                             : (radioState == RadioState::BAND_MISSING
                                    ? "BAND / RANGE"
                                    : "RADIO RETRY");
    snprintf(buf,sizeof(buf),"%s%s%s",
             haveTime?hm:"",haveTime?"  ":"",status);
  }
  tft.drawString(buf,W-6,5);
}
// mini 1-hour sparkline with warning reference lines
void miniPlot(int x,int y,int w,int h,bool isHR){
  uint8_t* data=isHR?hrHist:spHist;
  float ymin=isHR?50:80, ymax=isHR?250:100;        // BPM clips 50..250 ; SpO2 80..100
  float refYel=isHR?160:92, refRed=isHR?200:90;    // yellow/red warning lines
  uint16_t col=isHR?0x6E6C:0x74FF;
  tft.fillRect(x,y,w,h,BG);            // clear our own area, so the live view never full-screen wipes
  tft.drawRect(x,y,w,h,LINE);
  // TOP_PAD reserves a strip for the label. Without it a reading at the top of the scale (SpO2 100,
  // HR 250) plots one pixel above the text and sits on top of "SpO2 1h".
  const int TOP_PAD=11;
  auto Y=[&](float v){ v=constrain(v,ymin,ymax);
                       return y+h-2-(int)((v-ymin)/(ymax-ymin)*(h-3-TOP_PAD)); };
  // NOTE: this panel renders TFT_YELLOW/TFT_RED swapped, so the constants are crossed on purpose:
  tft.drawFastHLine(x+1,Y(refYel),w-2,TFT_RED);      // caution line -> shows YELLOW
  tft.drawFastHLine(x+1,Y(refRed),w-2,TFT_YELLOW);   // danger line  -> shows RED
  tft.setFont(&fonts::Font0); tft.setTextColor(GREY); tft.setTextDatum(textdatum_t::top_left);
  tft.drawString(isHR?"BPM 1h":"SpO2 1h",x+3,y+2);
  int n=histCnt;
  if(n>=2){ int px=-1,py=-1;
    for(int i=0;i<n;i++){ int idx=(histHead-n+i+HN)%HN; int v=data[idx];
      int xx=x+1+(int)((long)i*(w-3)/(n-1)); int yy=Y(v);
      if(px>=0) tft.drawLine(px,py,xx,yy,col); px=xx; py=yy; }
  }
}
// Repaints only the regions whose values actually changed.
//
// This used to fillScreen() and redraw everything whenever the render key changed - which is on
// every new measurement, about every 22 s (measured: 32 logged rows in 12 minutes). Wiping the
// whole panel to change two numbers reads as a blink. Each region now clears its own rectangle
// instead, and the full wipe happens only on a genuine full redraw: theme change, or returning
// from another view.
void drawLive(const ReadingSnapshot& reading,RadioState radioState,bool stale,uint8_t alertMask,
              const LiveRenderKey& key,const LiveRenderKey& prev,bool full){
  bool d=stale;
  if(full){ tft.fillScreen(BG); tft.drawFastHLine(0,HDR-2,W,LINE); }

  if(full || key.minuteKey!=prev.minuteKey || key.signal!=prev.signal ||
     key.radioState!=prev.radioState || key.stale!=prev.stale ||
     key.skinTenths!=prev.skinTenths || key.skinValid!=prev.skinValid){
    drawHeader(reading,radioState,stale);       // already clears its own strip
  }

  char heartText[8];
  char oxygenText[8];
  if(stale || reading.heartRate == 0) strlcpy(heartText,"--",sizeof(heartText));
  else snprintf(heartText,sizeof(heartText),"%d",reading.heartRate);
  if(stale || reading.oxygenSaturation == 0) strlcpy(oxygenText,"--",sizeof(oxygenText));
  else snprintf(oxygenText,sizeof(oxygenText),"%d",reading.oxygenSaturation);
  // top row: big current values
  if(full || key.heartRate!=prev.heartRate || key.stale!=prev.stale)
    cell(0,0,"HEART",heartText,"bpm",d?DIM:((reading.heartRate<HR_LOW||reading.heartRate>HR_HIGH)?TFT_RED:0x6E6C));
  if(full || key.oxygenSaturation!=prev.oxygenSaturation || key.stale!=prev.stale)
    cell(1,0,"OXYGEN",oxygenText,"%",d?DIM:((reading.oxygenSaturation&&reading.oxygenSaturation<SPO2_LOW)?TFT_RED:0x74FF));
  // bottom row: 1-hour sparklines. Redrawn when the alert bar appears or clears too, because the
  // bar overlaps their bottom rows and leaves a hole behind it otherwise.
  int py=HDR+RH;
  if(full || key.sequence!=prev.sequence || key.alertMask!=prev.alertMask){
    miniPlot(2,      py+2, COLW-3, RH-4, true);   // BPM
    miniPlot(COLW+1, py+2, COLW-3, RH-4, false);  // SpO2
  }
  char alertText[48] = "ALERT:";
  if(alertMask & ALERT_HR_LOW) strlcat(alertText," HR LOW",sizeof(alertText));
  if(alertMask & ALERT_HR_HIGH) strlcat(alertText," HR HIGH",sizeof(alertText));
  if(alertMask & ALERT_SPO2_LOW) strlcat(alertText," SpO2 LOW",sizeof(alertText));
  if(alertMask != ALERT_NONE){ int by=H-20; tft.fillRect(0,by,W,20,TFT_RED);
    tft.setFont(&fonts::FreeSansBold9pt7b); tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(textdatum_t::middle_center); tft.drawString(alertText,W/2,by+10); }
}
void renderLive(const ReadingSnapshot& reading,RadioState radioState){
  uint32_t ageMs=static_cast<uint32_t>(millis()-reading.lastPacketMs);
  bool stale=reading.lastPacketMs==0 || ageMs>STALE_MS;
  uint8_t alertMask=makeAlertMask(reading.heartRate,reading.oxygenSaturation,
                                  stale,HR_LOW,HR_HIGH,SPO2_LOW);
  LiveRenderKey key=makeLiveRenderKey(reading,radioState,stale,alertMask,minuteOfDay());
  if(!g_renderKeyValid || key!=g_lastRenderKey){
    drawLive(reading,radioState,stale,alertMask,key,g_lastRenderKey,!g_renderKeyValid);
    g_lastRenderKey=key;
    g_renderKeyValid=true;
  }
}

// ---------- 24h plot ----------
void drawPlot(){
  int metric=plotMetric;                // 0=HR, 1=SpO2
  int group=plotBinMin/15;              // 15-min units per plotted bin
  int nb=96/group;
  float ymin = metric?70:40, ymax = metric?100:200;
  uint16_t col = metric?0x74FF:0x6E6C;
  int PX0=34, PY0=30, PX1=W-6, PY1=H-24;
  tft.fillScreen(BG);
  // title (no Back button — auto-returns to the live view after 10 s of no touch)
  tft.setFont(&fonts::FreeSansBold9pt7b); tft.setTextColor(GREY); tft.setTextDatum(textdatum_t::top_center);
  tft.drawString(String(metric?"OXYGEN":"HEART")+" 24h ("+plotBinMin+"m)", W/2, 5);
  // axes frame
  tft.drawRect(PX0,PY0,PX1-PX0,PY1-PY0,LINE);
  // Y gridlines + labels
  tft.setFont(&fonts::Font0); tft.setTextColor(DIM);
  for(int k=0;k<=4;k++){ float v=ymin+(ymax-ymin)*k/4; int y=PY1-(int)((v-ymin)/(ymax-ymin)*(PY1-PY0));
    tft.drawFastHLine(PX0,y,PX1-PX0,GRID); tft.setTextDatum(textdatum_t::middle_right); tft.drawString(String((int)v),PX0-2,y); }
  // X gridlines + hour labels (0,6,12,18,24)
  for(int hh=0;hh<=24;hh+=6){ int x=PX0+(int)((float)hh/24*(PX1-PX0));
    tft.drawFastVLine(x,PY0,PY1-PY0,GRID); tft.setTextDatum(textdatum_t::top_center); tft.drawString(String(hh),x,PY1+2); }
  // bars: avg +/- std as error bars
  float bw=(float)(PX1-PX0)/nb;
  for(int i=0;i<nb;i++){
    uint32_t n=0; float s=0,sq=0;
    for(int j=0;j<group;j++){ Bin& b=bins[i*group+j];
      if(metric){ n+=b.nO; s+=b.sO; sq+=b.sqO; } else { n+=b.nH; s+=b.sH; sq+=b.sqH; } }
    if(n==0) continue;
    float avg=s/n; float var=sq/n-avg*avg; if(var<0)var=0; float sd=sqrtf(var);
    int cx=PX0+(int)((i+0.5f)*bw);
    auto Y=[&](float v){ v=constrain(v,ymin,ymax); return PY1-(int)((v-ymin)/(ymax-ymin)*(PY1-PY0)); };
    int yl=Y(avg-sd), yh=Y(avg+sd), ya=Y(avg);
    tft.drawFastVLine(cx,yh,yl-yh,col&0x7BEF);   // std band (dim)
    tft.fillCircle(cx,ya,2,col);                 // avg dot
  }
  tft.setTextColor(DIM); tft.setTextDatum(textdatum_t::bottom_right);
  tft.setFont(&fonts::Font0); tft.drawString("tap: change bin  -  auto-back 10s",W-4,H-2);
}

// Swap the chrome palette when the night window opens or closes, then repaint.
// Value colours are untouched on purpose - the numbers keep the same hues day and night.
void applyTheme(){
  static int cur=-1; int n=isNight()?1:0;
  if(n==cur) return; cur=n;
  BG=n?INV(BG_D):BG_D;      GREY=n?INV(GREY_D):GREY_D;  DIM=n?INV(DIM_D):DIM_D;
  LINE=n?INV(LINE_D):LINE_D; GRID=n?INV(GRID_D):GRID_D;
  g_renderKeyValid=false;                         // invalidate the live-view redraw cache
  tft.fillScreen(BG);
  if(view==PLOT) drawPlot();
}

// ---------- touch ----------
uint16_t xpt(uint8_t cmd){
  digitalWrite(T_CS,LOW);
  for(int i=7;i>=0;i--){ digitalWrite(T_MOSI,(cmd>>i)&1); digitalWrite(T_CLK,HIGH); digitalWrite(T_CLK,LOW); }
  uint16_t v=0; digitalWrite(T_CLK,HIGH); digitalWrite(T_CLK,LOW);
  for(int i=11;i>=0;i--){ digitalWrite(T_CLK,HIGH); v|=(digitalRead(T_MISO)<<i); digitalWrite(T_CLK,LOW); }
  digitalWrite(T_CS,HIGH); return v;
}
bool touchXY(int& sx,int& sy){
  uint16_t z=xpt(0xB0); if(z<250) return false;
  uint32_t ax=0,ay=0; for(int i=0;i<5;i++){ ax+=xpt(0x90); ay+=xpt(0xD0);} ax/=5; ay/=5;
  sx=constrain((int)map(ax,TOUCH_X0,TOUCH_X1,0,W),0,W-1);
  sy=constrain((int)map(ay,TOUCH_Y0,TOUCH_Y1,0,H),0,H-1);
  return true;
}

// ---------- data export: SoftAP + a tiny read-only HTTP server ----------
WebServer* g_http=nullptr;

// Whitelist, never a path mapping: /wifi.txt on the same card holds the HOME network password in
// plain text, and this access point is reachable by anyone in the room while it is up.
static bool exportAllowed(const String& p){
  return p.startsWith("/vitals_") && p.endsWith(".csv") && p.indexOf("..")<0;
}
// Index of available days, so the page can offer a range without guessing filenames.
static void handleDays(){
  String j="["; File dir=SD.open("/");
  if(dir){
    for(File e=dir.openNextFile(); e; e=dir.openNextFile()){
      String n=e.name(); if(!n.startsWith("/")) n="/"+n;
      if(exportAllowed(n)){
        if(j.length()>1) j+=",";
        j+="{\"f\":\""+n+"\",\"n\":"+String((uint32_t)e.size())+"}";
      }
      e.close();
    }
    dir.close();
  }
  j+="]"; g_http->send(200,"application/json",j);
}
static void handleFile(){
  String p=g_http->uri();
  if(!exportAllowed(p)){ g_http->send(404,"text/plain","not found"); return; }
  File f=SD.open(p);
  if(!f){ g_http->send(404,"text/plain","not found"); return; }
  g_http->streamFile(f,"text/csv"); f.close();
}

void drawExportStatic(){
  tft.fillScreen(BG);
  tft.setTextDatum(textdatum_t::top_center);
  tft.setFont(&fonts::FreeSansBold9pt7b); tft.setTextColor(GREY);
  tft.drawString("DATA EXPORT",W/2,5);
  tft.drawFastHLine(0,24,W,LINE);
  struct Row { const char* label; const char* value; } rows[] = {
    {"Wi-Fi network", AP_SSID},
    {"Password",      AP_PASS},
    {"Open in browser","http://192.168.4.1"},
  };
  int y=34;
  for(auto& r : rows){
    tft.setFont(&fonts::FreeSans9pt7b); tft.setTextColor(DIM);
    tft.setTextDatum(textdatum_t::top_center); tft.drawString(r.label,W/2,y);
    tft.setFont(&fonts::FreeSansBold12pt7b); tft.setTextColor(GREY);
    tft.drawString(r.value,W/2,y+15);
    y+=52;
  }
  // Monitoring really is off while this is up - say so in the same style as a vitals alert.
  int by=H-42; tft.fillRect(0,by,W,20,TFT_RED);
  tft.setFont(&fonts::FreeSansBold9pt7b); tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString("MONITORING PAUSED",W/2,by+10);
  tft.setFont(&fonts::FreeSans9pt7b); tft.setTextColor(DIM);
  tft.setTextDatum(textdatum_t::top_center);
  tft.drawString("swipe down to resume",W/2,by+24);
}
// Only the countdown changes, so redraw just that strip once a second.
void drawExportCountdown(){
  uint32_t left = (millis()-g_exportStart >= EXPORT_TIMEOUT_MS) ? 0
                : (EXPORT_TIMEOUT_MS-(millis()-g_exportStart))/1000;
  char b[56]; snprintf(b,56,"auto-resume in %lu:%02lu  (%d client%s)",
                       (unsigned long)left/60,(unsigned long)left%60,
                       WiFi.softAPgetStationNum(), WiFi.softAPgetStationNum()==1?"":"s");
  tft.fillRect(0,H-16,W,16,BG);
  tft.setFont(&fonts::Font0); tft.setTextColor(DIM);
  tft.setTextDatum(textdatum_t::top_center); tft.drawString(b,W/2,H-13);
}

void enterExport(){
  view=EXPORT; g_exportStart=millis();
  BLEDevice::deinit(true);                       // hand the radio over before WiFi starts
  WiFi.mode(WIFI_AP);
  if(!WiFi.softAP(AP_SSID,AP_PASS)){             // no AP means no export: get back to monitoring
    Serial.println("[export] softAP failed"); delay(200); ESP.restart();
  }
  g_http=new WebServer(80);
  g_http->on("/",[](){ g_http->send_P(200,"text/html",APP_HTML); });
  g_http->on("/days",handleDays);
  g_http->onNotFound(handleFile);
  g_http->begin();
  setBacklight(BRIGHT_DAY);                      // the credentials have to be readable at night too
  drawExportStatic(); drawExportCountdown();
  Serial.printf("[export] AP=%s ip=%s\n",AP_SSID,WiFi.softAPIP().toString().c_str());
}
// Reboot rather than tear down: restarting is the one reliable way back to a full-speed BLE radio
// after WiFi has run (same reason setup() reboots after its NTP sync). Clock and history survive.
void exitExport(){
  Serial.println("[export] resuming monitoring");
  if(g_http){ g_http->stop(); }
  WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF);
  delay(150); ESP.restart();
}

// Gestures resolve on lift-off, not on contact: a swipe begins as a touch, so acting on the first
// sample would fire the tap action at the start of every swipe.
// Returns 0 none, 1 tap (position in tx,ty), 2 swipe up, 3 swipe down.
bool g_touching=false;
// The one touch sample taken per loop pass. readGesture resolves taps and swipes on lift-off;
// the three-second hold needs the same raw sample while the finger is still down, and sampling
// the panel twice a pass to get it would be pure waste.
bool g_touchDown=false; int g_touchX=0, g_touchY=0;
int readGesture(int& tx,int& ty){
  static int x0,y0,x1,y1; static uint32_t t0=0;
  int sx,sy; bool now=touchXY(sx,sy);
  g_touchDown=now; if(now){ g_touchX=sx; g_touchY=sy; }
  if(now){
    if(!g_touching){ g_touching=true; x0=x1=sx; y0=y1=sy; t0=millis(); }
    else { x1=sx; y1=sy; }
    return 0;
  }
  if(!g_touching) return 0;
  g_touching=false;
  int dx=x1-x0, dy=y1-y0;
  if(abs(dy)>=SWIPE_MIN_DY && abs(dy)>abs(dx)) return dy<0 ? 2 : 3;
  if(abs(dx)<=TAP_MAX_MOVE && abs(dy)<=TAP_MAX_MOVE && millis()-t0<600){ tx=x0; ty=y0; return 1; }
  return 0;                                      // too short for a swipe, too smeared for a tap
}

void bootMsg(const char* s){ tft.fillScreen(BG); tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(GREY); tft.setTextDatum(textdatum_t::middle_center); tft.drawString(s,tft.width()/2,tft.height()/2); }

void setup(){
  Serial.begin(115200); delay(300);
  // Latch the true cause before the NTP soft-reboot can overwrite it with ESP_RST_SW (see g_origReason).
  { esp_reset_reason_t rr=esp_reset_reason(); if(rr!=ESP_RST_SW) g_origReason=rr; }
  pinMode(T_CLK,OUTPUT); pinMode(T_MOSI,OUTPUT); pinMode(T_CS,OUTPUT);
  pinMode(T_MISO,INPUT); pinMode(T_IRQ,INPUT); digitalWrite(T_CS,HIGH); digitalWrite(T_CLK,LOW);
  tft.init();
  for(int r=0;r<4;r++){ tft.setRotation(r); tft.fillScreen(BG); }
  tft.setRotation(ROTATION); tft.setBrightness(BRIGHT_DAY);   // daytime level until the clock is known
  W=tft.width(); H=tft.height(); HDR=28; COLW=W/2; RH=(H-HDR)/2;
  setenv("TZ",TZ_INFO,1); tzset();   // re-apply TZ each boot (survives via env, not the reboot)
  bootMsg("SD card..."); initSD();
  // Clock: WiFi coexistence badly throttles BLE, and a deinit can't fully undo it. So on a COLD
  // boot we sync NTP once, then SOFT-REBOOT into BLE-only mode (the RTC clock survives the
  // reboot). On the second boot the time is already set, WiFi is skipped, and BLE runs full speed.
  struct tm tmc{};
  bool haveTime = readLocalClock(tmc);
  if (!haveTime && !g_syncedThisPower) {
    bootMsg("WiFi clock sync (one-time)...");
    bool synced = syncTimeOverWifi();
    if (synced && readLocalClock(tmc)) {
      g_syncedThisPower = true;                     // survives the soft reboot (RTC memory)
      bootMsg("clock set - rebooting for BLE...");
      delay(250);
      ESP.restart();
    }
  }
  g_timeReady = readLocalClock(tmc);
  logBoot();                                      // evidence for tomorrow morning: why did it restart?
  applyBrightness(); applyTheme();
  bootMsg("Loading history..."); loadCsvToday();
  loadContacts(); reconcileOpenEpisode();
  if(g_alarm.state==AlarmState::ALARM){ view=ALARM; setBacklight(255); }
  bootMsg("Bluetooth...");
  BLEDevice::init(""); g_bleScanner=BLEDevice::getScan();
  g_bleScanner->setAdvertisedDeviceCallbacks(new CB(),true);
  // Passive: never transmit a scan request. Required by the receive-only rule at the top of this
  // file, and it also saves the TX bursts. Everything we decode is in the advertisement itself.
  startBleScan(millis());
  tft.fillScreen(BG);
  Serial.printf("[ready] sd=%d time=%d\n",g_sdReady,g_timeReady);
}

// Entering the alarm: the screen is taken over and brightness forced up regardless of night mode,
// and the onset is on the card before anything else can go wrong.
void onAlarmStarted(const ReadingSnapshot& r){
  g_selfTest=false;                 // a real alarm during a self-test must not be labelled TEST
  view=ALARM; setBacklight(255); g_renderKeyValid=false; g_alarmKeyValid=false;
  logAlert("ONSET",r.heartRate,r.oxygenSaturation,alarmCauseName(g_alarm.cause));
  Serial.printf("[alarm] %s hr=%d\n",alarmCauseName(g_alarm.cause),r.heartRate);
}

void leaveAlarmView(){
  g_selfTest=false; view=LIVE; applyBrightness(); applyTheme();
  g_renderKeyValid=false; g_alarmKeyValid=false; tft.fillScreen(BG);
}

// The alarm owns the screen but not the radio: unlike export mode, BLE keeps running underneath,
// so the trace and the numbers stay live while the alarm is up.
void serviceAlarmView(const ReadingSnapshot& reading,RadioState radioState,bool stale){
  const HoldResult hold=holdUpdate(g_hold,g_touchDown,g_touchX,g_touchY,millis(),
                                   HOLD_COUNTDOWN_MS,TAP_MAX_MOVE);

  if(hold.completed){
    if(!g_selfTest){
      logAlert("DISMISS",reading.heartRate,reading.oxygenSaturation,"dismissed");
      alarmDismiss(g_alarm,millis());
    }
    leaveAlarmView(); return;
  }
  if(g_selfTest && static_cast<uint32_t>(millis()-g_selfTestStart)>=SELFTEST_DURATION_S*1000UL){
    leaveAlarmView(); return;
  }

  const uint32_t nowE=nowEpochOrZero();
  AlarmAppearance a;
  a.machine=&g_alarm; a.contacts=&g_contacts;
  a.heartRate=reading.heartRate; a.oxygen=reading.oxygenSaturation;
  a.oxygenAgeMin = (g_spo2StampEpoch && nowE>=g_spo2StampEpoch)
                     ? static_cast<int>((nowE-g_spo2StampEpoch)/60) : -1;
  a.elapsedS = g_selfTest ? 0 : alarmElapsedS(g_alarm,nowE);
  a.selfTest=g_selfTest;
  a.selfTestLeftS=static_cast<int>(SELFTEST_DURATION_S-(millis()-g_selfTestStart)/1000);
  a.stale=stale; a.radioState=radioState;

  // Three independent repaints, so the fastest-changing thing does not drag the slowest through
  // a redraw: the vitals and trace only when a new reading lands (~20 s), the timer once a
  // second in its own corner, the frame twice a second as six thin rectangles.
  const int key = reading.sequence*7 + reading.heartRate + reading.oxygenSaturation*3
                  + (stale?9001:0) + static_cast<int>(radioState)*37;
  if(!g_alarmKeyValid || key!=g_alarmKey){
    g_alarmKey=key; g_alarmKeyValid=true;
    drawAlarmStatic(a); drawAlarmTimer(a); drawAlarmHold(hold,g_selfTest);
    // Evidence for "the trace looks frozen": prints once per repaint, so the interval between
    // lines is the real reading cadence and hist/timed say whether the trace has a time axis.
    Serial.printf("[alarm] draw hr=%d spo2=%d seq=%d hist=%d timed=%d age=%lus\n",
                  reading.heartRate,reading.oxygenSaturation,reading.sequence,histCnt,
                  tracePositional(histEpoch,histCnt)?0:1,
                  (unsigned long)(reading.lastPacketMs?(millis()-reading.lastPacketMs)/1000:0));
  }

  static uint32_t lastTimerS=0xFFFFFFFF;
  const uint32_t showS = g_selfTest ? (uint32_t)a.selfTestLeftS : a.elapsedS;
  if(showS!=lastTimerS){ lastTimerS=showS; drawAlarmTimer(a); }

  static bool lastFrame=false, lastHoldActive=false; static uint8_t lastPct=255;
  const bool frameOn=(millis()/ALARM_FLASH_MS)%2==0;
  if(frameOn!=lastFrame){ lastFrame=frameOn; drawAlarmFrame(frameOn); }
  if(hold.active!=lastHoldActive || hold.percent!=lastPct){
    lastHoldActive=hold.active; lastPct=hold.percent; drawAlarmHold(hold,g_selfTest);
  }
}

void loop(){
  static uint32_t lastTouch=0;
  // Always sampled, even when the alarm owns the screen, so g_touchDown/g_touchX/g_touchY are
  // fresh for the hold below and the panel is only read once per pass. The gesture result itself
  // is ignored while the alarm is up - see the view guards on each handler.
  int tx=0,ty=0; int g = readGesture(tx,ty);
  const bool alarmOwned = (view==ALARM || g_selfTest);

  // Export mode owns the loop: no BLE, no live view, just serve the page until told to stop.
  if(view==EXPORT){
    g_http->handleClient();
    if(g==3 || millis()-g_exportStart>EXPORT_TIMEOUT_MS) exitExport();   // reboots, never returns
    static uint32_t lastCd=0;
    if(millis()-lastCd>1000){ lastCd=millis(); drawExportCountdown(); }
    delay(5);                                    // keep the server responsive
    return;
  }
  // Swipe up -> share the logged data, but never during an alarm: enterExport() deinitialises
  // Bluetooth and replaces the screen, which would hide the alarm and stop the readings feeding it.
  if(g==2 && view==LIVE && g_alarm.state!=AlarmState::ALARM){ enterExport(); return; }

  // Hold the HEART cell to run the alarm self-check. This resolves while still touching, unlike
  // readGesture()'s tap, because the countdown has to be drawn during the hold; holdConsumedTap
  // then suppresses the release so testing the alarm does not also open the 24-hour chart.
  // Guarded against g_selfTest: during a self-test the view is still LIVE, and without this both
  // this block and serviceAlarmView would drive the same HoldState in one pass.
  bool holdingHeart=false;
  if(view==LIVE && !g_selfTest){
    const bool onHeart = g_touchDown && g_touchY>=HDR && g_touchX<COLW;
    const HoldResult h=holdUpdate(g_hold,onHeart,g_touchX,g_touchY,millis(),HOLD_COUNTDOWN_MS,TAP_MAX_MOVE);
    if(h.completed){
      g_selfTest=true; g_selfTestStart=millis(); setBacklight(255);
      g_alarmKeyValid=false; Serial.println("[alarm] self-test");
      return;
    }
    static bool wasHolding=false;
    if(h.active){                               // same countdown the alarm screen uses
      holdingHeart=true;                        // suppress renderLive, or it erases the digit
      char c[8]; snprintf(c,sizeof(c),"%u",(unsigned)h.secondsLeft);
      tft.setFont(&fonts::FreeSansBold9pt7b);
      tft.setTextColor(TFT_YELLOW,BG);          // opaque background so 3->2->1 overwrites cleanly
      tft.setTextDatum(textdatum_t::middle_center); tft.drawString(c,COLW/2,HDR+RH/2);
      wasHolding=true;
    } else if(wasHolding){                      // abandoned: repaint the live view over the digit
      wasHolding=false; g_renderKeyValid=false;
    }
    if(g==1 && holdConsumedTap(g_hold)) g=0;
  }

  // Gesture ACTIONS are suppressed while the alarm owns the screen - the sample above is still
  // taken, but a tap must not open the 24-hour chart on top of a running alarm or self-test.
  if(g==1 && !alarmOwned){
    lastTouch=millis();
    if(view==LIVE){
      if(ty>=HDR){                              // tap a column (number or its sparkline) -> 24h chart
        plotMetric = (tx<COLW)?0:1; view=PLOT; drawPlot();
      }
    } else if(view==PLOT){ // any tap cycles the bin size
      plotBinMin = (plotBinMin==60)?30:(plotBinMin==30)?15:60; drawPlot();
    }
  }
  // auto-return to the live view after 10 s of no touch in the plot
  if(view==PLOT && millis()-lastTouch>10000){ view=LIVE; g_renderKeyValid=false; tft.fillScreen(BG); }
  RadioState radioState = serviceRadioRecovery(millis());
  ReadingSnapshot reading = readSnapshot();

  // ---- critical alarm ----
  // Driven every pass, before rendering, so the alarm can take the screen in the same pass it
  // fires rather than a frame later.
  const bool stale = reading.lastPacketMs==0 ||
                     static_cast<uint32_t>(millis()-reading.lastPacketMs)>STALE_MS;
  {
    static int lastAlarmSeq=-1;
    if(reading.sequence!=lastAlarmSeq && reading.lastPacketMs!=0 &&
       static_cast<uint32_t>(millis()-reading.lastPacketMs)<3000){
      lastAlarmSeq=reading.sequence;
      const uint32_t nowE=nowEpochOrZero();      // one clock read per reading, not three
      if(reading.oxygenSaturation>0) g_spo2StampEpoch=nowE;
      AlarmEvent ev=alarmOnReading(g_alarm,ALARM_CFG,reading.heartRate,millis(),nowE);
      if(ev.alarmStarted) onAlarmStarted(reading);
      if(ev.episodeResolved) logAlert("RESOLVED",reading.heartRate,reading.oxygenSaturation,"");
    }
    AlarmEvent tick=alarmTick(g_alarm,ALARM_CFG,millis(),stale);
    if(tick.alarmStarted) onAlarmStarted(reading);
  }

  if(view==ALARM || g_selfTest){
    serviceAlarmView(reading,radioState,stale);
    // Fall through to logging below so the CSV and the trace keep filling behind the alarm.
  }
  else if(view==LIVE && !holdingHeart) renderLive(reading,radioState);

  // day/night backlight, checked every 10 s (no-op unless the level actually changes).
  // Skipped while the alarm is up: it forces full brightness regardless of the hour, and letting
  // night mode dim it back down would be the single worst bug this feature could have.
  static uint32_t lastBl=0;
  if(view!=ALARM && !g_selfTest && millis()-lastBl>10000){
    lastBl=millis(); applyBrightness(); applyTheme();
  }

  // log + bin new readings
  static int lastLogged=-1;
  if(reading.sequence!=lastLogged && reading.lastPacketMs != 0 &&
     static_cast<uint32_t>(millis()-reading.lastPacketMs)<3000){
    lastLogged=reading.sequence;
    logRow(reading.heartRate,reading.oxygenSaturation,reading.skinC,reading.skinValid);
    int mod=minuteOfDay(); if(mod>=0) addReading(reading.heartRate,reading.oxygenSaturation,mod);
    pushHist(reading.heartRate,reading.oxygenSaturation,nowEpochOrZero());  // feed the 1-hour sparklines
  }
  delay(g_touching?15:60);                       // sample faster mid-gesture so swipes track well
}
