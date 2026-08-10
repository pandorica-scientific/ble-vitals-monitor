// cyd_vitals.ino — Baby Sensor Relax monitor + logger (CYD ESP32-2432S028).
// RECEIVE-ONLY passive BLE. Never connects/writes/pairs -> base link untouched.
//
// Display: big HR + SpO2, skin temp small by the name, 1-hour BPM/SpO2 sparklines with
//   yellow/red warning lines. Tap a column -> 24h avg±std chart (tap cycles 1h/30m/15m;
//   auto-returns to live after 10 s of no touch).
// Also: WiFi+NTP clock (Europe/Warsaw) synced once at boot then reboots BLE-only (coexistence),
//   per-reading CSV logging to microSD (one file per day), and a backlight that drops to its
//   lowest step between 20:00 and 08:00 so it doesn't light up the room at night.
// Power: the BLE scan is duty-cycled and the daytime backlight runs below full, because the
//   whole board lives off a USB power bank (see SCAN_* and BRIGHT_DAY below).
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
#include "app_html.h"      // the report page served in export mode (PROGMEM)

#define ROTATION 0
#define HR_LOW 90
#define HR_HIGH 180
#define SPO2_LOW 90
#define SKIN_MIN 28.0f
#define SKIN_MAX 42.0f
#define STALE_MS 30000
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
// ---- power-bank keep-alive (v2) ----
// Most USB power banks cut their output when the load stays under ~50-100 mA, and they judge that
// on the AVERAGE current over a multi-second window, not on peaks.
// v1 tried a 120 ms burst every 8 s. That is 1.5% duty: even a generous +50 mA burst lifts the
// average by ~0.75 mA, so the bank never noticed and still cut out overnight. Peaks do not work;
// only a genuinely higher sustained average does.
// v2 therefore holds an artificial load for a large fraction of every slice, around the clock.
// Both loads are silent and invisible - no backlight flash, so the room stays dark at night:
//   - a CPU spin, which also stops loop()'s delay() parking the core in its low-power idle
//   - repeated 512 B reads of today's CSV. READ-only, so it costs no write wear on the card.
// The load runs in short slices rather than one long block so touch stays responsive.
// Cost: this deliberately burns current, so a bank that is NOT on mains pass-through will run flat
// sooner. Rough order of magnitude on a 10 Ah bank, ~6 Ah usable at 5 V: several days either way,
// so the trade is worth it - but if you ever run this off a charge rather than a socket, expect it.
#define KEEPALIVE_DUTY_PCT  60    // percent of each slice spent under load. 0 = off.
                                  // Start high: another failed night costs a whole night of data.
                                  // Once it survives, step down 60 -> 45 -> 30 to find the margin.
#define KEEPALIVE_SLICE_MS  200   // load/idle slice. 60% -> 120 ms load, 80 ms normal loop work.
#define KEEPALIVE_NIGHT_ONLY 0    // 0 = run around the clock. The daytime backlight probably carries
                                  // the load on its own, but "probably" is not worth a missed night:
                                  // a bank that cuts out at noon loses just as much data.
// At night the BLE radio also goes to 100% duty: interval == window. Receiving is one of the few
// big silent loads available, and it doubles as better overnight capture - fewer "--" dropouts at
// exactly the hours the monitor matters most. Costs ~40 mA, which is the point.
#define SCAN_WINDOW_NIGHT_MS SCAN_INTERVAL_MS
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
volatile int   g_hr=0, g_spo2=0, g_sig=0, g_rssi=0, g_seq=-1;
volatile float g_skin=0;
volatile bool  g_skinValid=false;
volatile uint32_t g_lastMs=0;
bool g_timeReady=false, g_sdReady=false;
RTC_DATA_ATTR bool g_syncedThisPower=false;   // survives the soft reboot below
// The real reason this power cycle started, carried across the deliberate NTP soft-reboot below.
// Without this every restart would be recorded as SW: the cold boot has no clock yet, so it cannot
// write a timestamped line, and by the time boot 2 can, esp_reset_reason() only reports our own
// ESP.restart(). RTC memory survives a SW reset but not a power cut, which is exactly the
// distinction logBoot() needs.
RTC_DATA_ATTR esp_reset_reason_t g_origReason=ESP_RST_UNKNOWN;
// NOT g_scan: the ESP32 WiFi blob (libnet80211.a) exports a global of that exact name and the
// link fails with a multiple-definition error. Same trap as the TX1 macro clash.
BLEScan* g_bleScan=nullptr;                   // kept so the night duty cycle can be retuned live

// ---------- rolling ~1-hour history for the mini sparklines (~180 readings @ ~20s) ----------
#define HN 180
uint8_t hrHist[HN], spHist[HN]; int histHead=0, histCnt=0;
void pushHist(int hr,int sp){
  hrHist[histHead]=constrain(hr,0,254); spHist[histHead]=constrain(sp,0,254);
  histHead=(histHead+1)%HN; if(histCnt<HN) histCnt++;
}

class CB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (!dev.haveManufacturerData()) return;
    String m = dev.getManufacturerData();
    if (m.length() < 23) return;
    const uint8_t* b = (const uint8_t*)m.c_str();
    if (b[0]!=0xF5 || b[1]!=0x03) return;
    g_seq=b[2]; g_hr=b[10]; g_spo2=b[13]; g_sig=b[4]; g_rssi=dev.getRSSI();
    // Skin temp = big-endian uint16 at bytes 6-7, divided by 10 (0.1C resolution). CONFIRMED.
    // (byte 3 bit 0x08 flags a freshly-generated temperature; updates ~every 15 min.)
    float sk = (((uint16_t)b[6]<<8) | b[7]) / 10.0f;
    if (sk>=SKIN_MIN && sk<=SKIN_MAX) { g_skin=sk; g_skinValid=true; }
    g_lastMs=millis();
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
bool initSD() {
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  g_sdReady = SD.begin(SD_CS, sdSPI) && SD.cardType()!=CARD_NONE;
  Serial.println(g_sdReady?"[SD] OK":"[SD] FAILED"); return g_sdReady;
}
void syncTimeOverWifi() {
  if (!g_sdReady) return;
  File f=SD.open("/wifi.txt"); if(!f){Serial.println("[wifi.txt] missing");return;}
  String ssid=f.readStringUntil('\n'); ssid.trim();
  String pass=f.readStringUntil('\n'); pass.trim(); f.close();
  if(!ssid.length()) return;
  WiFi.mode(WIFI_STA); WiFi.begin(ssid.c_str(),pass.c_str());
  uint32_t t0=millis(); while(WiFi.status()!=WL_CONNECTED && millis()-t0<20000) delay(250);
  if(WiFi.status()==WL_CONNECTED){
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3","pool.ntp.org","time.nist.gov");
    struct tm tm; t0=millis(); while(!getLocalTime(&tm,200)&&millis()-t0<15000) delay(200);
    if(getLocalTime(&tm)){ g_timeReady=true; char b[24]; strftime(b,24,"%F %T",&tm); Serial.printf("[ntp] %s\n",b);}
  } else Serial.println("[wifi] FAILED");
  // Fully release the radio to BLE (WIFI_OFF alone can leave coexistence throttling BLE)
  WiFi.disconnect(true,true); WiFi.mode(WIFI_OFF); esp_wifi_stop(); esp_wifi_deinit();
}
// One definition of the log filename: the writer (logRow) and the reader (loadCsvToday) must never
// disagree about which file "today" is.
void csvName(char* out,const struct tm* tm){ strftime(out,32,"/vitals_%Y-%m-%d.csv",tm); }
void csvName(char* out){ struct tm tm; getLocalTime(&tm); csvName(out,&tm); }
void logRow(int hr,int spo2,float skin,bool sv){
  if(!g_sdReady||!g_timeReady) return; struct tm tm; if(!getLocalTime(&tm)) return;
  char fn[32]; csvName(fn,&tm); bool isNew=!SD.exists(fn);
  File f=SD.open(fn,FILE_APPEND); if(!f) return;
  if(isNew) f.println("timestamp,hr_bpm,spo2_pct,skin_c");
  char ts[24]; strftime(ts,24,"%F %T",&tm);
  if(sv) f.printf("%s,%d,%d,%.1f\n",ts,hr,spo2,skin); else f.printf("%s,%d,%d,\n",ts,hr,spo2);
  f.close();
}
void loadCsvToday(){
  if(!g_sdReady||!g_timeReady) return; char fn[32]; csvName(fn);
  File f=SD.open(fn); if(!f){Serial.println("[csv] no file to load");return;}
  f.readStringUntil('\n'); int n=0;
  while(f.available()){
    String ln=f.readStringUntil('\n'); if(ln.length()<19) continue;
    int hh=ln.substring(11,13).toInt(), mm=ln.substring(14,16).toInt();
    int c1=ln.indexOf(','); int c2=ln.indexOf(',',c1+1); int c3=ln.indexOf(',',c2+1);
    if(c1<0||c2<0) continue;
    int hr=ln.substring(c1+1,c2).toInt();
    int sp=ln.substring(c2+1,c3<0?ln.length():c3).toInt();
    addReading(hr,sp,hh*60+mm); pushHist(hr,sp); n++;   // last HN kept -> ~1h sparkline survives reboot
    // remember the last valid skin temp so the display holds it (like the app) instead of "--"
    if(c3>=0){ String sf=ln.substring(c3+1); sf.trim();
      if(sf.length()){ float sk=sf.toFloat(); if(sk>=SKIN_MIN&&sk<=SKIN_MAX){ g_skin=sk; g_skinValid=true; } } }
  }
  f.close(); Serial.printf("[csv] loaded %d rows into bins (heldSkin=%.1f valid=%d)\n",n,g_skin,g_skinValid);
}
// Why the board restarted, appended to its own file so the CSV format the report app parses is
// untouched. There is no current meter on this build, so this is the only evidence that separates
// the three candidate causes of a dead screen in the morning:
//   POWERON  - the supply was cut and came back    -> power bank idle-cutoff, keep-alive too weak
//   BROWNOUT - the 5 V rail sagged                 -> cable/bank current limit, NOT an idle cutoff
//   PANIC/WDT/TASK_WDT - firmware crashed          -> a bug, and no keep-alive setting will fix it
// A single POWERON at the time you plugged it in is normal. Extra entries overnight are the bug.
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
  struct tm tm; char ts[24]="?";
  if(getLocalTime(&tm)) strftime(ts,24,"%F %T",&tm);
  f.printf("%s,%s,%d\n",ts,r,(int)g_origReason); f.close();
  Serial.printf("[boot] %s reset=%s(%d)\n",ts,r,(int)g_origReason);
}
bool nowHM(char* o){ struct tm tm; if(!getLocalTime(&tm))return false; strftime(o,8,"%H:%M",&tm); return true; }
int minuteOfDay(){ struct tm tm; if(!getLocalTime(&tm))return -1; return tm.tm_hour*60+tm.tm_min; }
// 20:00 -> 08:00 is "night": dim backlight + dark theme.
// Gated on g_timeReady: with no clock getLocalTime() blocks, and the day look is the safe default.
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

// Hold a silent artificial load so the power bank keeps seeing a real device (see KEEPALIVE_* above).
// Called once per loop(); each call either runs one load slice or returns immediately.
void keepAlive(){
  if(!KEEPALIVE_DUTY_PCT) return;
  if(KEEPALIVE_NIGHT_ONLY && !isNight()) return;   // by day the backlight already draws plenty
  static uint32_t last=0;
  if(millis()-last < KEEPALIVE_SLICE_MS) return;   // idle part of the slice: let loop() do its work
  last=millis();

  // Read-only handle on today's log. Reopened per slice on purpose: the file the writer appends to
  // changes at midnight, and a stale handle would pin the previous day's file open all night.
  File f; if(g_sdReady && g_timeReady){ char fn[32]; csvName(fn); f=SD.open(fn); }
  uint8_t buf[512]; volatile float spin=1.0f;
  uint32_t on=(uint32_t)KEEPALIVE_SLICE_MS*KEEPALIVE_DUTY_PCT/100, t0=millis();
  while(millis()-t0 < on){
    if(f){ if(!f.available()) f.seek(0); f.read(buf,sizeof(buf)); }
    for(int i=0;i<2000;i++) spin=spin*1.000001f+0.5f;
  }
  if(f) f.close();
}

// 100% BLE duty while dimmed, the measured 50% duty by day. Only touches the radio on an actual
// day/night transition, i.e. twice a day.
// The scan MUST be stopped and restarted: setWindow() only writes BLEScan::m_scan_params, and those
// are pushed to the controller by esp_ble_gap_set_scan_params() inside start(). Calling setWindow()
// on a running scan changes nothing at all - it fails silently, which is why this is worth a comment.
void applyScanDuty(){
  if(!g_bleScan) return;
  static int cur=-1; int n=isNight()?1:0;
  if(n==cur) return; cur=n;
  g_bleScan->stop();
  g_bleScan->setInterval(SCAN_INTERVAL_MS);
  g_bleScan->setWindow(n?SCAN_WINDOW_NIGHT_MS:SCAN_WINDOW_MS);
  g_bleScan->start(0,nullptr,false);
  Serial.printf("[keepalive] %s: scan %d/%d ms, load duty=%d%%\n",
                n?"night":"day", n?SCAN_WINDOW_NIGHT_MS:SCAN_WINDOW_MS, SCAN_INTERVAL_MS,
                KEEPALIVE_DUTY_PCT);
}

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
enum View { LIVE, PLOT, EXPORT };
View view=LIVE; int plotMetric=0; int plotBinMin=60;   // 60/30/15
uint32_t g_exportStart=0;                              // millis() when export mode began
String g_lastSig="~"; long g_lastAge=-999;             // live-view redraw state (global so we can force it)

void cell(int col,int r,const char* label,const String& val,const char* unit,uint16_t color){
  int x=col*COLW,y=HDR+r*RH;
  tft.setTextDatum(textdatum_t::top_left); tft.setFont(&fonts::FreeSans9pt7b); tft.setTextColor(GREY);
  tft.drawString(label,x+8,y+2);
  tft.setTextDatum(textdatum_t::bottom_right); tft.drawString(unit,x+COLW-8,y+RH-4);
  tft.setFont(&fonts::Font7); tft.setTextSize(1); tft.setTextColor(color);
  tft.setTextDatum(textdatum_t::top_left); tft.drawString(val,x+8,y+20);
}
void drawHeader(bool stale,int sig,int rssi,long ageS){
  tft.fillRect(0,2,W,HDR-4,BG);
  tft.setFont(&fonts::FreeSansBold9pt7b); tft.setTextDatum(textdatum_t::top_left);
  tft.setTextColor(GREY); tft.drawString("Oliwia",6,4);
  if(g_skinValid && !stale){ char sb[10]; snprintf(sb,10,"%.1fC",g_skin);   // skin temp small, by the name
    tft.setFont(&fonts::FreeSans9pt7b); tft.setTextColor(0xFE79); tft.setTextDatum(textdatum_t::top_left);
    tft.drawString(sb,74,5); }
  char hm[8]; bool haveT=nowHM(hm);
  tft.setFont(&fonts::FreeSans9pt7b); tft.setTextDatum(textdatum_t::top_right); char buf[40];
  if(stale){tft.setTextColor(0xEB44); snprintf(buf,40,"%s %s",haveT?hm:"",g_lastMs?"NO SIGNAL":"scan");}
  else {tft.setTextColor(GREY); snprintf(buf,40,"%s  sig%d",haveT?hm:"",sig);}
  tft.drawString(buf,W-6,5);
}
// mini 1-hour sparkline with warning reference lines
void miniPlot(int x,int y,int w,int h,bool isHR){
  uint8_t* data=isHR?hrHist:spHist;
  float ymin=isHR?50:80, ymax=isHR?250:100;        // BPM clips 50..250 ; SpO2 80..100
  float refYel=isHR?160:92, refRed=isHR?200:90;    // yellow/red warning lines
  uint16_t col=isHR?0x6E6C:0x74FF;
  tft.drawRect(x,y,w,h,LINE);
  auto Y=[&](float v){ v=constrain(v,ymin,ymax); return y+h-2-(int)((v-ymin)/(ymax-ymin)*(h-3)); };
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
void drawLive(bool stale,int hr,int spo2,float skin,bool tempOK,int sig,int rssi,long ageS,const String& alert){
  bool d=stale; tft.fillScreen(BG); tft.drawFastHLine(0,HDR-2,W,LINE);
  drawHeader(stale,sig,rssi,ageS);
  // top row: big current values
  cell(0,0,"HEART", (d||!hr)?"--":String(hr),   "bpm", d?DIM:((hr<HR_LOW||hr>HR_HIGH)?TFT_RED:0x6E6C));
  cell(1,0,"OXYGEN",(d||!spo2)?"--":String(spo2),"%",   d?DIM:((spo2&&spo2<SPO2_LOW)?TFT_RED:0x74FF));
  // bottom row: 1-hour sparklines
  int py=HDR+RH;
  miniPlot(2,      py+2, COLW-3, RH-4, true);   // BPM
  miniPlot(COLW+1, py+2, COLW-3, RH-4, false);  // SpO2
  if(alert.length()){ int by=H-20; tft.fillRect(0,by,W,20,TFT_RED);
    tft.setFont(&fonts::FreeSansBold9pt7b); tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(textdatum_t::middle_center); tft.drawString("ALERT:"+alert,W/2,by+10); }
}
void renderLive(){
  uint32_t age=millis()-g_lastMs; bool stale=(g_lastMs==0)||(age>STALE_MS);
  int hr=g_hr,spo2=g_spo2,sig=g_sig,rssi=g_rssi; float skin=g_skin; bool tempOK=g_skinValid;
  long ageS=stale?-1:(long)(age/1000); String alert="";
  if(!stale){ if(hr&&hr<HR_LOW)alert+=" HR LOW"; if(hr>HR_HIGH)alert+=" HR HIGH"; if(spo2&&spo2<SPO2_LOW)alert+=" SpO2 LOW"; }
  String s=String(stale)+","+hr+","+spo2+","+String(skin,1)+","+g_seq+","+alert;  // g_seq -> refresh sparklines each reading
  if(s!=g_lastSig){ drawLive(stale,hr,spo2,skin,tempOK,sig,rssi,ageS,alert); g_lastSig=s; g_lastAge=ageS; }
  else if(ageS!=g_lastAge){ drawHeader(stale,sig,rssi,ageS); g_lastAge=ageS; }
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
  g_lastSig="~";                                 // invalidate the live-view redraw cache
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
int readGesture(int& tx,int& ty){
  static int x0,y0,x1,y1; static uint32_t t0=0;
  int sx,sy; bool now=touchXY(sx,sy);
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
  setenv("TZ","CET-1CEST,M3.5.0,M10.5.0/3",1); tzset();   // re-apply TZ each boot (survives via env, not the reboot)
  bootMsg("SD card..."); initSD();
  // Clock: WiFi coexistence badly throttles BLE, and a deinit can't fully undo it. So on a COLD
  // boot we sync NTP once, then SOFT-REBOOT into BLE-only mode (the RTC clock survives the
  // reboot). On the second boot the time is already set, WiFi is skipped, and BLE runs full speed.
  struct tm tmc; bool haveTime = getLocalTime(&tmc) && tmc.tm_year>120;
  if (!haveTime && !g_syncedThisPower) {
    g_syncedThisPower = true;                       // survives the soft reboot (RTC memory)
    bootMsg("WiFi clock sync (one-time)..."); syncTimeOverWifi();
    if (getLocalTime(&tmc) && tmc.tm_year>120) { bootMsg("clock set - rebooting for BLE..."); delay(250); ESP.restart(); }
  }
  g_timeReady = getLocalTime(&tmc) && tmc.tm_year>120;
  logBoot();                                      // evidence for tomorrow morning: why did it restart?
  applyBrightness(); applyTheme();
  bootMsg("Loading history..."); loadCsvToday();
  bootMsg("Bluetooth...");
  BLEDevice::init(""); BLEScan* scan=BLEDevice::getScan(); g_bleScan=scan;
  scan->setAdvertisedDeviceCallbacks(new CB(),true);
  // Passive: never transmit a scan request. Required by the receive-only rule at the top of this
  // file, and it also saves the TX bursts. Everything we decode is in the advertisement itself.
  scan->setActiveScan(false);
  scan->setInterval(SCAN_INTERVAL_MS); scan->setWindow(SCAN_WINDOW_MS);
  scan->start(0,nullptr,false);
  applyScanDuty();                                // go straight to night duty if we booted after dark
  tft.fillScreen(BG);
  Serial.printf("[ready] sd=%d time=%d\n",g_sdReady,g_timeReady);
}

void loop(){
  static uint32_t lastTouch=0;
  int tx=0,ty=0; int g=readGesture(tx,ty);

  // Export mode owns the loop: no BLE, no live view, just serve the page until told to stop.
  if(view==EXPORT){
    g_http->handleClient();
    if(g==3 || millis()-g_exportStart>EXPORT_TIMEOUT_MS) exitExport();   // reboots, never returns
    static uint32_t lastCd=0;
    if(millis()-lastCd>1000){ lastCd=millis(); drawExportCountdown(); }
    delay(5);                                    // keep the server responsive
    return;
  }
  if(g==2 && view==LIVE){ enterExport(); return; }   // swipe up -> share the logged data

  if(g==1){
    lastTouch=millis();
    if(view==LIVE){
      if(ty>=HDR){                              // tap a column (number or its sparkline) -> 24h chart
        plotMetric = (tx<COLW)?0:1; view=PLOT; drawPlot();
      }
    } else { // PLOT: any tap cycles the bin size
      plotBinMin = (plotBinMin==60)?30:(plotBinMin==30)?15:60; drawPlot();
    }
  }
  // auto-return to the live view after 10 s of no touch in the plot
  if(view==PLOT && millis()-lastTouch>10000){ view=LIVE; g_lastSig="~"; tft.fillScreen(BG); }
  if(view==LIVE) renderLive();

  // day/night backlight, checked every 10 s (no-op unless the level actually changes)
  static uint32_t lastBl=0;
  if(millis()-lastBl>10000){ lastBl=millis(); applyBrightness(); applyTheme(); applyScanDuty(); }

  // Stop the power bank cutting out on the dim night load. Skipped mid-gesture: the load slice
  // blocks for ~120 ms, which is too coarse to track a swipe, and someone touching the screen at
  // 3 a.m. wants the UI responsive far more than they want the next slice of current.
  if(!g_touching) keepAlive();

  // log + bin new readings
  static int lastLogged=-1;
  if(g_seq!=lastLogged && g_lastMs && (millis()-g_lastMs)<3000){
    lastLogged=g_seq;
    logRow(g_hr,g_spo2,g_skin,g_skinValid);
    int mod=minuteOfDay(); if(mod>=0) addReading(g_hr,g_spo2,mod);
    pushHist(g_hr,g_spo2);                       // feed the 1-hour sparklines
  }
  delay(g_touching?15:60);                       // sample faster mid-gesture so swipes track well
}
