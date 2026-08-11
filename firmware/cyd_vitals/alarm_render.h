#pragma once

// Alarm screen. Included by cyd_vitals.ino AFTER tft, W, H, DIM and the history buffer exist -
// this header deliberately does not include the graphics library itself, matching how the sketch
// already shares its display state.
//
// PANEL QUIRK: this unit renders TFT_RED and TFT_YELLOW swapped, so everything that must appear
// red uses TFT_YELLOW. Same crossing as miniPlot().
//
// Drawing is split three ways so the flash does not repaint the whole panel. A full fillScreen at
// the flash rate flickers badly and makes the numbers hard to read - which defeats the point of
// putting emergency numbers on the screen at all.
//   drawAlarmStatic - everything that changes only when a value changes
//   drawAlarmFrame  - the six-pixel border, repainted twice a second
//   drawAlarmHold   - the countdown band, repainted while a finger is down

#define ALARM_RED    TFT_YELLOW   // crossed: see the panel quirk above
#define ALARM_DIMRED 0x4000
#define ALARM_TEXT   TFT_WHITE
#define ALARM_GAP    0x39E7

#define ALARM_FRAME_PX 6
#define ALARM_TRACE_Y  78
#define ALARM_TRACE_H  84
#define ALARM_HOLD_Y   178
#define ALARM_HOLD_H   26
#define ALARM_CONTACT_Y 208
#define ALARM_TIMER_W    66   // room for "59:59" at FreeSansBold9pt7b, and no more

struct AlarmAppearance {
  const AlarmMachine* machine = nullptr;
  const Contacts* contacts = nullptr;
  int heartRate = 0;
  int oxygen = 0;
  int oxygenAgeMin = -1;    // -1 when unknown
  uint32_t elapsedS = 0;
  bool selfTest = false;
  int selfTestLeftS = 0;
};

// Only the perimeter flashes. A flashing background makes the telephone numbers and the trace
// unreadable, and a frame is more visible from across a room than a header band anyway.
inline void drawAlarmFrame(bool on){
  const int t=ALARM_FRAME_PX; const uint16_t c = on ? ALARM_RED : ALARM_DIMRED;
  tft.fillRect(0,0,W,t,c); tft.fillRect(0,H-t,W,t,c);
  tft.fillRect(0,0,t,H,c); tft.fillRect(W-t,0,t,H,c);
}

inline void drawAlarmTrace(int x,int y,int w,int h,uint32_t elapsedS){
  tft.fillRect(x,y,w,h,TFT_BLACK);
  const uint32_t windowMin = traceWindowMinutes(elapsedS);
  const uint32_t spanS = windowMin*60;
  const int lo=60, hi=260;                    // fixed, so a rising rate cannot look flat

  const int ty = y+h-((HR_CRIT-lo)*h)/(hi-lo);
  for(int px=x; px<x+w; px+=6) tft.drawFastHLine(px,ty,3,ALARM_DIMRED);
  tft.setFont(&fonts::Font0); tft.setTextColor(ALARM_DIMRED);
  tft.setTextDatum(textdatum_t::bottom_right); tft.drawString("200",x+w-2,ty-1);

  char axis[24];
  tft.setTextColor(DIM); tft.setTextDatum(textdatum_t::top_right);
  tft.drawString("now",x+w,y+h+2);

  if(histCnt<2){ tft.setTextDatum(textdatum_t::top_left); tft.drawString("no history yet",x,y+h+2); return; }

  const bool positional = tracePositional(histEpoch,histCnt);
  uint32_t newest=0;
  for(int i=0;i<histCnt;i++){ const int k=(histHead-1-i+HN*2)%HN; if(histEpoch[k]>newest) newest=histEpoch[k]; }
  const bool timed = !positional && newest!=0;

  if(timed) snprintf(axis,sizeof(axis),"last %u min",(unsigned)windowMin);
  else      snprintf(axis,sizeof(axis),"last %d readings",histCnt);
  tft.setTextDatum(textdatum_t::top_left); tft.drawString(axis,x,y+h+2);

  int prevX=-1, prevY=-1; uint32_t prevE=0;
  for(int i=histCnt-1;i>=0;i--){
    const int k=(histHead-1-i+HN*2)%HN;
    const int hr=hrHist[k]; const uint32_t e=histEpoch[k];
    if(hr<=0){ prevX=-1; continue; }
    int px;
    if(!timed) px = x + (w*(histCnt-1-i))/(histCnt>1?histCnt-1:1);
    else {
      if(newest-e>spanS){ prevX=-1; prevE=e; continue; }
      px = x + w - (int)((newest-e)*(uint32_t)w/spanS);
    }
    int v=hr; if(v<lo) v=lo; if(v>hi) v=hi;
    const int py = y+h-((v-lo)*h)/(hi-lo);
    if(prevX>=0){
      // A dropout is a shaded band, not a line and not blank space. A line across a four-minute
      // gap reads as a steady rate for four minutes; blank space reads as "not filled up yet".
      // The band says the signal was lost over exactly that span.
      if(timed && prevE!=0 && e!=0 && traceIsGap(prevE,e)) tft.fillRect(prevX,y,px-prevX,h,ALARM_GAP);
      else tft.drawLine(prevX,prevY,px,py,ALARM_RED);
    }
    prevX=px; prevY=py; prevE=e;
  }
}

inline void drawAlarmStatic(const AlarmAppearance& a){
  const int t=ALARM_FRAME_PX;
  tft.fillRect(t,t,W-2*t,H-2*t,TFT_BLACK);

  tft.setFont(&fonts::FreeSansBold9pt7b); tft.setTextColor(ALARM_TEXT);
  tft.setTextDatum(textdatum_t::top_left);
  tft.drawString(a.selfTest ? "TEST - SELF-CHECK" : "HIGH HEART RATE", 10, 10);

  char hr[12];
  // The byte cannot express more than 255, so at the rail the true rate is unknown and at least
  // this high. Saying ">=255" is honest; printing 255 is not.
  if(a.heartRate>=HR_RAIL) snprintf(hr,sizeof(hr),">=255");
  else if(a.heartRate<=0)  snprintf(hr,sizeof(hr),"--");
  else snprintf(hr,sizeof(hr),"%d",a.heartRate);
  tft.setFont(&fonts::FreeSansBold12pt7b); tft.setTextColor(ALARM_RED);
  tft.setTextDatum(textdatum_t::top_left); tft.drawString(hr,10,34);

  tft.setFont(&fonts::Font0); tft.setTextColor(DIM);
  tft.setTextDatum(textdatum_t::top_left); tft.drawString("bpm",10,62);
  char pk[20]; snprintf(pk,sizeof(pk),"peak %d",a.machine?a.machine->peak:0);
  tft.drawString(pk,46,62);

  char sp[28];
  // Oxygen commits about every fifteen minutes, so the value beside a live heart rate may be a
  // quarter of an hour old. Without its age someone reads a stale number down the telephone.
  if(a.oxygen<=0) snprintf(sp,sizeof(sp),"SpO2 --");
  else if(a.oxygenAgeMin<0) snprintf(sp,sizeof(sp),"SpO2 %d%%",a.oxygen);
  else snprintf(sp,sizeof(sp),"SpO2 %d%% - %dm ago",a.oxygen,a.oxygenAgeMin);
  tft.setTextColor(a.oxygenAgeMin>=SPO2_STALE_MIN?DIM:ALARM_TEXT);
  tft.setTextDatum(textdatum_t::top_right); tft.drawString(sp,W-10,62);

  drawAlarmTrace(10,ALARM_TRACE_Y,W-20,ALARM_TRACE_H,a.elapsedS);

  if(a.contacts && a.contacts->count>0){
    tft.setFont(&fonts::Font0); tft.setTextColor(ALARM_TEXT);
    tft.setTextDatum(textdatum_t::top_center);
    for(int i=0;i<a.contacts->count;i++) tft.drawString(a.contacts->line[i],W/2,ALARM_CONTACT_Y+i*11);
  }
}

// The elapsed timer ticks once a second. Repainting it on its own keeps that tick from dragging
// the trace and the phone numbers through a full redraw every second, which strobes and makes
// them hard to read - the one thing this screen cannot afford.
inline void drawAlarmTimer(const AlarmAppearance& a){
  char t2[16];
  if(a.selfTest) snprintf(t2,sizeof(t2),"%ds",a.selfTestLeftS);
  else snprintf(t2,sizeof(t2),"%02u:%02u",(unsigned)(a.elapsedS/60),(unsigned)(a.elapsedS%60));
  // Only as wide as "59:59" needs. Clearing from W/2 would wipe the tail off the title beside it -
  // "HIGH HEART RATE" and "TEST - SELF-CHECK" both run past the middle of a 320 px panel.
  tft.fillRect(W-ALARM_TIMER_W-ALARM_FRAME_PX,ALARM_FRAME_PX,ALARM_TIMER_W,26,TFT_BLACK);
  tft.setFont(&fonts::FreeSansBold9pt7b); tft.setTextColor(ALARM_TEXT);
  tft.setTextDatum(textdatum_t::top_right); tft.drawString(t2,W-10,10);
}

// The countdown is not only feedback. It makes accidental dismissal essentially impossible -
// three seconds of deliberate contact is not something a sleeve or a shifting blanket produces -
// and it makes it obvious the board has not frozen.
inline void drawAlarmHold(const HoldResult& hold, bool selfTest){
  const int t=ALARM_FRAME_PX;
  tft.fillRect(t,ALARM_HOLD_Y,W-2*t,ALARM_HOLD_H,TFT_BLACK);
  tft.setTextDatum(textdatum_t::top_center);
  if(hold.active){
    tft.setFont(&fonts::Font0); tft.setTextColor(DIM);
    tft.drawString(selfTest?"HOLD TO TEST ALARM":"HOLD TO DISMISS",W/2,ALARM_HOLD_Y);
    char c[8]; snprintf(c,sizeof(c),"%u",(unsigned)hold.secondsLeft);
    tft.setFont(&fonts::FreeSansBold9pt7b); tft.setTextColor(ALARM_TEXT);
    tft.drawString(c,W/2,ALARM_HOLD_Y+10);
    const int bw=(W-2*t-20)*hold.percent/100;
    tft.fillRect(t+10,ALARM_HOLD_Y+ALARM_HOLD_H-3,bw,3,ALARM_RED);
  } else if(!selfTest){
    tft.setFont(&fonts::Font0); tft.setTextColor(DIM);
    tft.drawString("HOLD 3s TO DISMISS",W/2,ALARM_HOLD_Y+8);
  }
}
