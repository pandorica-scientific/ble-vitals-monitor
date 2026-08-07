// esp32_cyd_vitals.ino — Baby Sensor Relax monitor + logger (CYD ESP32-2432S028).
// RECEIVE-ONLY passive BLE. Never connects/writes/pairs -> base link untouched.
//
// Display: big HR + SpO2, skin temp small by the name, 1-hour BPM/SpO2 sparklines with
//   yellow/red warning lines. Tap a column -> 24h avg±std chart (tap cycles 1h/30m/15m;
//   auto-returns to live after 10 s of no touch).
// Also: WiFi+NTP clock (Europe/Warsaw) synced once at boot then reboots BLE-only (coexistence),
//   and per-reading CSV logging to microSD (one file per day).
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
#include <time.h>
#include <math.h>

#define ROTATION 0
#define HR_LOW 90
#define HR_HIGH 180
#define SPO2_LOW 90
#define SKIN_MIN 28.0f
#define SKIN_MAX 42.0f
#define STALE_MS 30000
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
// touch calibration (from 5-point cal)
#define TX0 170
#define TX1 3840
#define TY0 320
#define TY1 3760

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
void csvName(char* out){ struct tm tm; getLocalTime(&tm); strftime(out,32,"/vitals_%Y-%m-%d.csv",&tm); }
void logRow(int hr,int spo2,float skin,bool sv){
  if(!g_sdReady||!g_timeReady) return; struct tm tm; if(!getLocalTime(&tm)) return;
  char fn[32]; strftime(fn,32,"/vitals_%Y-%m-%d.csv",&tm); bool isNew=!SD.exists(fn);
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
bool nowHM(char* o){ struct tm tm; if(!getLocalTime(&tm))return false; strftime(o,8,"%H:%M",&tm); return true; }
int minuteOfDay(){ struct tm tm; if(!getLocalTime(&tm))return -1; return tm.tm_hour*60+tm.tm_min; }

// ---------- UI ----------
const uint16_t GREY=0x9CD3, DIM=0x52AA, LINE=0x2965;
int W,H,HDR,COLW,RH;
enum View { LIVE, PLOT };
View view=LIVE; int plotMetric=0; int plotBinMin=60;   // 60/30/15
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
  tft.fillRect(0,2,W,HDR-4,TFT_BLACK);
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
  bool d=stale; tft.fillScreen(TFT_BLACK); tft.drawFastHLine(0,HDR-2,W,LINE);
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
  tft.fillScreen(TFT_BLACK);
  // title (no Back button — auto-returns to the live view after 10 s of no touch)
  tft.setFont(&fonts::FreeSansBold9pt7b); tft.setTextColor(GREY); tft.setTextDatum(textdatum_t::top_center);
  tft.drawString(String(metric?"OXYGEN":"HEART")+" 24h ("+plotBinMin+"m)", W/2, 5);
  // axes frame
  tft.drawRect(PX0,PY0,PX1-PX0,PY1-PY0,LINE);
  // Y gridlines + labels
  tft.setFont(&fonts::Font0); tft.setTextColor(DIM);
  for(int k=0;k<=4;k++){ float v=ymin+(ymax-ymin)*k/4; int y=PY1-(int)((v-ymin)/(ymax-ymin)*(PY1-PY0));
    tft.drawFastHLine(PX0,y,PX1-PX0,0x1082); tft.setTextDatum(textdatum_t::middle_right); tft.drawString(String((int)v),PX0-2,y); }
  // X gridlines + hour labels (0,6,12,18,24)
  for(int hh=0;hh<=24;hh+=6){ int x=PX0+(int)((float)hh/24*(PX1-PX0));
    tft.drawFastVLine(x,PY0,PY1-PY0,0x1082); tft.setTextDatum(textdatum_t::top_center); tft.drawString(String(hh),x,PY1+2); }
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
  sx=constrain((int)map(ax,TX0,TX1,0,W),0,W-1);
  sy=constrain((int)map(ay,TY0,TY1,0,H),0,H-1);
  return true;
}

void bootMsg(const char* s){ tft.fillScreen(TFT_BLACK); tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(GREY); tft.setTextDatum(textdatum_t::middle_center); tft.drawString(s,tft.width()/2,tft.height()/2); }

void setup(){
  Serial.begin(115200); delay(300);
  pinMode(T_CLK,OUTPUT); pinMode(T_MOSI,OUTPUT); pinMode(T_CS,OUTPUT);
  pinMode(T_MISO,INPUT); pinMode(T_IRQ,INPUT); digitalWrite(T_CS,HIGH); digitalWrite(T_CLK,LOW);
  tft.init();
  for(int r=0;r<4;r++){ tft.setRotation(r); tft.fillScreen(TFT_BLACK); }
  tft.setRotation(ROTATION); tft.setBrightness(200);
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
  bootMsg("Loading history..."); loadCsvToday();
  bootMsg("Bluetooth...");
  BLEDevice::init(""); BLEScan* scan=BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new CB(),true);
  scan->setActiveScan(true); scan->setInterval(100); scan->setWindow(99); scan->start(0,nullptr,false);
  tft.fillScreen(TFT_BLACK);
  Serial.printf("[ready] sd=%d time=%d\n",g_sdReady,g_timeReady);
}

void loop(){
  // touch handling (debounced)
  static uint32_t lastTouch=0; int sx,sy;
  if(touchXY(sx,sy) && millis()-lastTouch>250){
    lastTouch=millis();
    if(view==LIVE){
      if(sy>=HDR){                              // tap a column (number or its sparkline) -> 24h chart
        plotMetric = (sx<COLW)?0:1; view=PLOT; drawPlot();
      }
    } else { // PLOT: any tap cycles the bin size
      plotBinMin = (plotBinMin==60)?30:(plotBinMin==30)?15:60; drawPlot();
    }
  }
  // auto-return to the live view after 10 s of no touch in the plot
  if(view==PLOT && millis()-lastTouch>10000){ view=LIVE; g_lastSig="~"; tft.fillScreen(TFT_BLACK); }
  if(view==LIVE) renderLive();

  // log + bin new readings
  static int lastLogged=-1;
  if(g_seq!=lastLogged && g_lastMs && (millis()-g_lastMs)<3000){
    lastLogged=g_seq;
    logRow(g_hr,g_spo2,g_skin,g_skinValid);
    int mod=minuteOfDay(); if(mod>=0) addReading(g_hr,g_spo2,mod);
    pushHist(g_hr,g_spo2);                       // feed the 1-hour sparklines
  }
  delay(60);
}
