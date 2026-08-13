#include "../firmware/cyd_vitals/band_protocol.h"
#include "../firmware/cyd_vitals/critical_alarm.h"
#include "../firmware/cyd_vitals/trace_window.h"
#include "../firmware/cyd_vitals/hold_gesture.h"
#include "../firmware/cyd_vitals/alert_log.h"
#include "../firmware/cyd_vitals/contacts.h"
#include "../firmware/cyd_vitals/live_render_key.h"
#include "../firmware/cyd_vitals/radio_health.h"
#include "../firmware/cyd_vitals/wifi_failover.h"
#include "../firmware/cyd_vitals/day_night.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int failures = 0;

static void expect(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

static void testBandDecoder() {
  const uint8_t frame[23] = {
      0xF5, 0x03, 0x2D, 0x03, 0x04, 0x28, 0x01, 0x70,
      0x01, 0x70, 0x6C, 0x02, 0x30, 0x62, 0x03, 0x36,
      0x33, 0x05, 0x6C, 0x4E, 0xCD, 0xF7, 0xA4};

  BandReading reading{};
  expect(decodeBandFrame(frame, sizeof(frame), reading), "known frame is accepted");
  expect(reading.sequence == 45, "sequence is byte 2");
  expect(reading.signal == 4, "signal is byte 4");
  expect(reading.heartRate == 108, "heart rate is byte 10");
  expect(reading.oxygenSaturation == 98, "oxygen saturation is byte 13");
  expect(std::fabs(reading.skinC - 36.8f) < 0.01f, "skin temperature is big-endian /10");
  expect(reading.skinValid, "36.8 C is plausible");

  expect(!decodeBandFrame(nullptr, sizeof(frame), reading), "null frame is rejected");
  expect(!decodeBandFrame(frame, 22, reading), "short frame is rejected");

  uint8_t wrongPrefix[23];
  for (size_t i = 0; i < sizeof(frame); ++i) wrongPrefix[i] = frame[i];
  wrongPrefix[0] = 0x00;
  expect(!decodeBandFrame(wrongPrefix, sizeof(wrongPrefix), reading), "wrong prefix is rejected");

  uint8_t coldSkin[23];
  for (size_t i = 0; i < sizeof(frame); ++i) coldSkin[i] = frame[i];
  coldSkin[6] = 0x00;
  coldSkin[7] = 0x64;
  expect(decodeBandFrame(coldSkin, sizeof(coldSkin), reading), "vitals survive implausible skin");
  expect(!reading.skinValid, "10.0 C skin is marked invalid");
}

static void testReadingMerge() {
  ReadingSnapshot previous{};
  previous.sequence = 44;
  previous.skinC = 36.8f;
  previous.skinValid = true;

  BandReading frame{};
  frame.sequence = 45;
  frame.signal = 4;
  frame.heartRate = 108;
  frame.oxygenSaturation = 98;
  frame.skinC = 10.0f;
  frame.skinValid = false;

  ReadingSnapshot next = mergeBandReading(previous, frame, -70, 12'345);
  expect(next.sequence == 45 && next.heartRate == 108, "new vitals are published together");
  expect(next.oxygenSaturation == 98 && next.signal == 4,
         "oxygen and signal belong to the same packet");
  expect(next.skinValid && std::fabs(next.skinC - 36.8f) < 0.01f, "invalid skin preserves held value");
  expect(next.rssi == -70 && next.lastPacketMs == 12'345, "metadata belongs to the same packet");

  frame.skinC = 37.2f;
  frame.skinValid = true;
  ReadingSnapshot warm = mergeBandReading(next, frame, -68, 12'500);
  expect(warm.skinValid && std::fabs(warm.skinC - 37.2f) < 0.01f,
         "valid skin replaces the held value");
}

static void testRadioHealth() {
  RadioHealthInput input{};
  input.nowMs = 10'000;
  input.scanStarted = true;
  input.scanStartedMs = 0;
  expect(classifyRadioHealth(input) == RadioState::STARTING, "startup grace is STARTING");

  input.bandSeen = true;
  input.lastBandMs = 9'500;
  expect(classifyRadioHealth(input) == RadioState::RECEIVING, "recent band wins during grace");

  input.nowMs = 40'000;
  input.bandSeen = false;
  input.anySeen = true;
  input.lastAnyMs = 39'500;
  expect(classifyRadioHealth(input) == RadioState::BAND_MISSING, "other traffic means band missing");

  input.anySeen = false;
  expect(classifyRadioHealth(input) == RadioState::SCANNER_SILENT, "no traffic after grace is silent");

  input.scanStarted = false;
  expect(classifyRadioHealth(input) == RadioState::SCANNER_SILENT, "failed start is silent");

  expect(shouldRestartScan(RadioState::SCANNER_SILENT, false, 40'000, 0), "first recovery is immediate");
  expect(!shouldRestartScan(RadioState::SCANNER_SILENT, true, 50'000, 40'000), "recovery is rate limited");
  expect(shouldRestartScan(RadioState::SCANNER_SILENT, true, 100'000, 40'000), "recovery resumes after 60 s");
  expect(!shouldRestartScan(RadioState::BAND_MISSING, false, 40'000, 0), "band loss never restarts scan");

  RadioHealthInput rollover{};
  rollover.scanStarted = true;
  rollover.scanStartedMs = UINT32_MAX - 40'000;
  rollover.anySeen = true;
  rollover.lastAnyMs = UINT32_MAX - 100;
  rollover.nowMs = 50;
  expect(classifyRadioHealth(rollover) == RadioState::BAND_MISSING, "millis rollover keeps recent traffic");
}

static void testWifiFailover() {
  std::vector<WifiSlot> attempts;
  bool result = runWifiFailover([&](WifiSlot slot) {
    attempts.push_back(slot);
    return slot == WifiSlot::BACKUP;
  });
  expect(result && attempts.size() == 2, "backup follows primary failure");
  expect(attempts[0] == WifiSlot::PRIMARY && attempts[1] == WifiSlot::BACKUP,
         "backup is the second slot");

  attempts.clear();
  result = runWifiFailover([&](WifiSlot slot) {
    attempts.push_back(slot);
    return slot == WifiSlot::BACKUP2;
  });
  expect(result && attempts.size() == 3, "backup2 follows two failures");
  expect(attempts[0] == WifiSlot::PRIMARY &&
         attempts[1] == WifiSlot::BACKUP &&
         attempts[2] == WifiSlot::BACKUP2,
         "slot order is primary then backup then backup2");

  attempts.clear();
  result = runWifiFailover([&](WifiSlot slot) {
    attempts.push_back(slot);
    return true;
  });
  expect(result && attempts.size() == 1 && attempts[0] == WifiSlot::PRIMARY,
         "primary success skips both backups");

  attempts.clear();
  result = runWifiFailover([&](WifiSlot slot) {
    attempts.push_back(slot);
    return false;
  });
  expect(!result && attempts.size() == 3,
         "three failures are returned after three attempts");

  expect(std::string(wifiCredentialPath(WifiSlot::PRIMARY)) == "/wifi.txt",
         "primary path is stable");
  expect(std::string(wifiCredentialPath(WifiSlot::BACKUP)) == "/wifi_backup.txt",
         "backup path is stable");
  expect(std::string(wifiCredentialPath(WifiSlot::BACKUP2)) == "/wifi_backup2.txt",
         "backup2 path is separate");
  expect(std::string(wifiCredentialTempPath(WifiSlot::BACKUP2)) == "/wifi_backup2.tmp",
         "backup2 temporary path is separate");
  expect(std::string(wifiCredentialRollbackPath(WifiSlot::BACKUP2)) == "/wifi_backup2.bak",
         "backup2 rollback path is separate");
  expect(std::string(wifiSlotLabel(WifiSlot::PRIMARY)) == "primary",
         "primary log label is non-secret");
  expect(std::string(wifiSlotLabel(WifiSlot::BACKUP)) == "backup",
         "backup log label is non-secret");
  expect(std::string(wifiSlotLabel(WifiSlot::BACKUP2)) == "backup2",
         "backup2 label is non-secret");

  WifiSlot parsed = WifiSlot::PRIMARY;
  expect(parseWifiSlot("primary", parsed) && parsed == WifiSlot::PRIMARY,
         "primary label parses");
  expect(parseWifiSlot("backup", parsed) && parsed == WifiSlot::BACKUP,
         "backup label parses");
  expect(parseWifiSlot("backup2", parsed) && parsed == WifiSlot::BACKUP2,
         "backup2 label parses");
  expect(!parseWifiSlot("unknown", parsed), "unknown label is rejected");
}

static void testLiveRenderKey() {
  ReadingSnapshot reading{};
  reading.sequence = 45;
  reading.heartRate = 108;
  reading.oxygenSaturation = 98;
  reading.skinC = 36.8f;
  reading.skinValid = true;

  uint8_t alerts = makeAlertMask(80, 95, false, 90, 180, 90);
  expect((alerts & ALERT_HR_LOW) != 0, "low heart rate alert is set");
  expect((alerts & ALERT_HR_HIGH) == 0, "high heart rate alert is clear");
  expect((alerts & ALERT_SPO2_LOW) == 0, "normal oxygen alert is clear");

  uint8_t highAndLowOxygen = makeAlertMask(181, 89, false, 90, 180, 90);
  expect((highAndLowOxygen & ALERT_HR_HIGH) != 0, "high heart rate alert is set");
  expect((highAndLowOxygen & ALERT_SPO2_LOW) != 0, "low oxygen alert is set");
  expect(makeAlertMask(80, 89, true, 90, 180, 90) == ALERT_NONE,
         "stale readings suppress alerts");

  LiveRenderKey first =
      makeLiveRenderKey(reading, RadioState::RECEIVING, false, alerts, 610);
  LiveRenderKey second =
      makeLiveRenderKey(reading, RadioState::RECEIVING, false, alerts, 610);
  expect(first == second, "identical render state is stable");

  second.sequence++;
  expect(first != second, "new sequence redraws sparklines");
  second = first;
  second.radioState = RadioState::BAND_MISSING;
  expect(first != second, "radio state redraws status");
  second = first;
  second.alertMask ^= ALERT_SPO2_LOW;
  expect(first != second, "alert change redraws warning");
  second = first;
  second.signal++;
  expect(first != second, "signal change redraws header");
  second = first;
  second.minuteKey++;
  expect(first != second, "new minute redraws clock");
}

// Feed readings 20 s apart while ticking every second, exactly as the main loop does, and run
// long enough for at least one confirmation window to close.
static AlarmMachine runWindow(const AlarmConfig& cfg, const std::vector<int>& readings) {
  AlarmMachine m{};
  const uint32_t epoch0 = 1'000'000;
  uint32_t endMs = static_cast<uint32_t>(readings.size()) * 20'000;
  if (endMs < cfg.confirmMs) endMs = cfg.confirmMs;
  endMs += 1'000;
  for (uint32_t t = 0; t <= endMs; t += 1'000) {
    const size_t idx = t / 20'000;
    if (t % 20'000 == 0 && idx < readings.size()) {
      alarmOnReading(m, cfg, readings[idx], t, epoch0 + t / 1000);
    }
    alarmTick(m, cfg, t, false);
  }
  return m;
}

static void testCriticalAlarm() {
  const AlarmConfig cfg{};

  // 1: one qualifying reading arms but does not alarm
  {
    AlarmMachine m{};
    alarmOnReading(m, cfg, 205, 0, 1'000'000);
    expect(m.state == AlarmState::ARMED, "one reading at 200 arms");
    alarmTick(m, cfg, 5'000, false);
    expect(m.state == AlarmState::ARMED, "still armed before the window closes");
  }

  // 2: sustained readings alarm, onset is the FIRST reading
  {
    AlarmMachine m = runWindow(cfg, {205, 205, 205, 205});
    expect(m.state == AlarmState::ALARM, "sustained 205 alarms");
    expect(m.cause == AlarmCause::CONFIRMED_HIGH, "cause is CONFIRMED_HIGH");
    expect(m.onsetEpoch == 1'000'000, "onset is the first qualifying reading");
  }

  // 3: a reading below cancel returns to IDLE
  {
    AlarmMachine m{};
    alarmOnReading(m, cfg, 205, 0, 1'000'000);
    alarmOnReading(m, cfg, 179, 20'000, 1'000'020);
    expect(m.state == AlarmState::IDLE, "179 cancels the window");
  }

  // 4: staleness while armed escalates
  {
    AlarmMachine m{};
    alarmOnReading(m, cfg, 205, 0, 1'000'000);
    AlarmEvent e = alarmTick(m, cfg, 30'000, true);
    expect(m.state == AlarmState::ALARM, "stale while armed alarms");
    expect(m.cause == AlarmCause::LOST_WHILE_CRITICAL, "cause is LOST_WHILE_CRITICAL");
    expect(e.alarmStarted, "the transition is reported once");
    AlarmEvent again = alarmTick(m, cfg, 31'000, true);
    expect(!again.alarmStarted, "repeated stale ticks do not re-fire");
  }

  // 5: staleness while idle does nothing
  {
    AlarmMachine m{};
    alarmOnReading(m, cfg, 140, 0, 1'000'000);
    alarmTick(m, cfg, 30'000, true);
    expect(m.state == AlarmState::IDLE, "stale while idle does not alarm");
  }

  // 6-17: window evaluation table
  struct WindowCase { std::vector<int> readings; bool alarms; const char* why; };
  const std::vector<WindowCase> cases = {
    {{205, 195, 185}, false, "falling and ends below sustain"},
    {{205, 185, 195}, true,  "rose again, ends above sustain"},
    {{205, 195, 195}, true,  "flat is not resolving"},
    {{205, 195, 196}, true,  "a one-beat rise is still not resolving"},
    {{205, 198, 193}, false, "strictly falling with one high reading"},
    {{205, 200, 195}, true,  "count path fires despite falling"},
    {{205, 185, 188}, false, "rose but ends below sustain"},
    {{205, 195, 205}, true,  "two high readings, ends high"},
    {{205, 205},      true,  "count is absolute, not a fraction"},
    {{205, 200, 190}, true,  "sustain boundary is inclusive"},
  };
  for (const WindowCase& c : cases) {
    AlarmMachine m = runWindow(cfg, c.readings);
    expect((m.state == AlarmState::ALARM) == c.alarms, c.why);
  }

  // a reading below crit never arms
  {
    AlarmMachine m{};
    alarmOnReading(m, cfg, 199, 0, 1'000'000);
    expect(m.state == AlarmState::IDLE, "199 never arms");
  }

  // a window completing on a single reading is treated as not resolving
  {
    AlarmMachine m{};
    alarmOnReading(m, cfg, 205, 0, 1'000'000);
    alarmTick(m, cfg, 60'000, false);
    expect(m.state == AlarmState::ALARM, "single-reading window fails toward alarming");
  }

  // an unconfirmed window restarts instead of resetting, keeping onset
  {
    AlarmMachine m = runWindow(cfg, {205, 198, 193});
    expect(m.state == AlarmState::ARMED, "unconfirmed window stays armed");
    expect(m.onsetEpoch == 1'000'000, "onset survives a window restart");
  }

  // oscillation never cancels and eventually alarms
  {
    AlarmMachine m = runWindow(cfg, {205, 195, 205, 195, 205, 205, 205});
    expect(m.state == AlarmState::ALARM, "oscillation eventually confirms");
    expect(m.onsetEpoch == 1'000'000, "onset is still the first crossing");
  }

  // implausible collapse alarms immediately, without a window
  {
    AlarmMachine m{};
    alarmOnReading(m, cfg, 190, 0, 1'000'000);
    AlarmEvent e = alarmOnReading(m, cfg, 24, 20'000, 1'000'020);
    expect(m.state == AlarmState::ALARM, "190 then 24 alarms at once");
    expect(m.cause == AlarmCause::IMPLAUSIBLE_COLLAPSE, "cause is IMPLAUSIBLE_COLLAPSE");
    expect(e.alarmStarted, "collapse reports the transition");
  }
  {
    AlarmMachine m{};
    alarmOnReading(m, cfg, 190, 0, 1'000'000);
    alarmOnReading(m, cfg, 85, 20'000, 1'000'020);
    expect(m.state == AlarmState::IDLE, "190 then 85 is not a collapse");
  }
  {
    AlarmMachine m{};
    alarmOnReading(m, cfg, 100, 0, 1'000'000);
    alarmOnReading(m, cfg, 40, 20'000, 1'000'020);
    expect(m.state == AlarmState::IDLE, "100 then 40 is not a collapse");
  }
  {
    AlarmMachine m{};
    alarmOnReading(m, cfg, 190, 0, 1'000'000);
    alarmOnReading(m, cfg, 0, 20'000, 1'000'020);
    expect(m.state == AlarmState::IDLE, "a zero reading is not a collapse");
  }

  // the alarm is terminal, peak tracks the maximum, resolution is reported once
  {
    AlarmMachine m = runWindow(cfg, {205, 221, 205, 205});
    expect(m.state == AlarmState::ALARM, "alarm is up");
    expect(m.peak == 221, "peak tracks the maximum");
    AlarmEvent e = alarmOnReading(m, cfg, 140, 100'000, 1'000'100);
    expect(m.state == AlarmState::ALARM, "a normal reading does not clear the alarm");
    expect(e.episodeResolved, "the drop below crit is reported once");
    AlarmEvent again = alarmOnReading(m, cfg, 138, 120'000, 1'000'120);
    expect(!again.episodeResolved, "resolution is reported only once");
  }

  // dismissal returns to IDLE and snoozes
  {
    AlarmMachine m = runWindow(cfg, {205, 205, 205, 205});
    alarmDismiss(m, 200'000);
    expect(m.state == AlarmState::IDLE, "dismissal returns to IDLE");
    AlarmMachine snoozed = m;
    alarmOnReading(snoozed, cfg, 210, 210'000, 1'000'210);
    for (uint32_t t = 0; t <= 80'000; t += 1'000) {
      alarmTick(snoozed, cfg, 210'000 + t, false);
    }
    expect(snoozed.state != AlarmState::ALARM, "no alarm within the snooze");
    AlarmMachine later = m;
    alarmOnReading(later, cfg, 210, 900'000, 1'001'000);
    for (uint32_t t = 0; t <= 80'000; t += 1'000) {
      alarmTick(later, cfg, 900'000 + t, false);
    }
    expect(later.state == AlarmState::ALARM, "alarm returns after the snooze");
  }

  // elapsed counts from onset
  {
    AlarmMachine m = runWindow(cfg, {205, 205, 205, 205});
    expect(alarmElapsedS(m, 1'000'396) == 396, "elapsed counts from onset");
  }
}

static void testTraceWindow() {
  expect(traceWindowMinutes(0) == 10, "a fresh episode uses a ten-minute window");
  expect(traceWindowMinutes(9 * 60) == 10, "nine minutes still fits ten");
  expect(traceWindowMinutes(12 * 60) == 30, "twelve minutes widens to thirty");
  expect(traceWindowMinutes(40 * 60) == 60, "forty minutes widens to sixty");
  expect(traceWindowMinutes(90 * 60) == 60, "ninety minutes stays capped at sixty");

  expect(!traceIsGap(1'000'000, 1'000'020), "twenty seconds apart is not a gap");
  expect(traceIsGap(1'000'000, 1'000'240), "four minutes apart is a gap");
  expect(!traceIsGap(1'000'000, 1'000'030), "exactly the stale limit is not yet a gap");

  expect(traceIsGap(1'000'000, 4'000'000'000u), "a huge difference is a gap, not an overflow");

  const uint32_t clean[3] = {1'000'000, 1'000'020, 1'000'040};
  expect(!tracePositional(clean, 3), "timestamped history uses a real time axis");
  const uint32_t withZero[3] = {0, 1'000'020, 1'000'040};
  expect(tracePositional(withZero, 3), "an unset clock forces the positional axis");
}

static void testHoldGesture() {
  const uint32_t HOLD = 3'000;
  const int MOVE = 20;

  // released early: nothing fires, no residual progress
  {
    HoldState h{};
    holdUpdate(h, true, 100, 100, 0, HOLD, MOVE);
    HoldResult r = holdUpdate(h, true, 100, 100, 2'900, HOLD, MOVE);
    expect(!r.completed, "a hold released at 2.9 s does not fire");
    expect(r.active && r.secondsLeft == 1, "the countdown shows one second left");
    HoldResult up = holdUpdate(h, false, 100, 100, 2'950, HOLD, MOVE);
    expect(!up.completed && !up.active, "releasing clears the hold");
    expect(up.percent == 0, "no residual progress remains");
  }

  // reaching the threshold fires exactly once
  {
    HoldState h{};
    holdUpdate(h, true, 100, 100, 0, HOLD, MOVE);
    HoldResult done = holdUpdate(h, true, 100, 100, 3'000, HOLD, MOVE);
    expect(done.completed, "a hold reaching 3.0 s fires");
    HoldResult more = holdUpdate(h, true, 100, 100, 3'500, HOLD, MOVE);
    expect(!more.completed, "it does not fire again while still held");
  }

  // progress advances monotonically and only reaches full at the end
  {
    HoldState h{};
    holdUpdate(h, true, 100, 100, 0, HOLD, MOVE);
    uint8_t last = 0;
    for (uint32_t t = 100; t < 3'000; t += 100) {
      HoldResult r = holdUpdate(h, true, 100, 100, t, HOLD, MOVE);
      expect(r.percent >= last, "progress never goes backwards");
      expect(r.percent < 100, "progress is not full before the end");
      last = r.percent;
    }
    HoldResult end = holdUpdate(h, true, 100, 100, 3'000, HOLD, MOVE);
    expect(end.percent == 100, "progress is full at the end");
  }

  // moving too far abandons the hold
  {
    HoldState h{};
    holdUpdate(h, true, 100, 100, 0, HOLD, MOVE);
    HoldResult moved = holdUpdate(h, true, 140, 100, 1'000, HOLD, MOVE);
    expect(!moved.active, "a drag abandons the hold");
    HoldResult later = holdUpdate(h, true, 140, 100, 3'500, HOLD, MOVE);
    expect(!later.completed, "an abandoned hold cannot complete");
  }

  // a completed hold suppresses the tap that would otherwise fire on release
  {
    HoldState h{};
    holdUpdate(h, true, 100, 100, 0, HOLD, MOVE);
    holdUpdate(h, true, 100, 100, 3'000, HOLD, MOVE);
    expect(holdConsumedTap(h), "a completed hold consumes the release");
    holdUpdate(h, false, 100, 100, 3'100, HOLD, MOVE);
    expect(!holdConsumedTap(h), "the flag clears once the finger is up");
  }
}

static void testAlertLog() {
  char row[128];
  formatAlertRow(row, sizeof(row), "2026-08-10 03:14:22", "ONSET", 214, 94, "CONFIRMED_HIGH");
  expect(std::string(row) == "2026-08-10 03:14:22,ONSET,214,94,CONFIRMED_HIGH",
         "an onset row is formatted exactly");

  formatAlertRow(row, sizeof(row), "2026-08-10 03:19:41", "RESOLVED", 176, 93, "");
  expect(std::string(row) == "2026-08-10 03:19:41,RESOLVED,176,93,",
         "an empty detail still emits its column");

  expect(std::string(alarmCauseName(AlarmCause::CONFIRMED_HIGH)) == "CONFIRMED_HIGH",
         "cause names are stable strings");
  expect(std::string(alarmCauseName(AlarmCause::LOST_WHILE_CRITICAL)) == "LOST_WHILE_CRITICAL",
         "lost-while-critical has a name");

  // an ONSET with no later DISMISS leaves an open episode
  {
    OpenEpisode s{};
    reconcileAlertLine("2026-08-10 03:14:22,ONSET,214,94,CONFIRMED_HIGH", 1'000'000, s);
    expect(s.open && s.onsetEpoch == 1'000'000, "an unmatched onset is open");
  }
  // a DISMISS closes it
  {
    OpenEpisode s{};
    reconcileAlertLine("2026-08-10 03:14:22,ONSET,214,94,CONFIRMED_HIGH", 1'000'000, s);
    reconcileAlertLine("2026-08-10 03:20:58,DISMISS,171,93,peak=221", 1'000'396, s);
    expect(!s.open, "a dismiss closes the episode");
  }
  // two closed episodes then an open third
  {
    OpenEpisode s{};
    reconcileAlertLine("a,ONSET,200,95,", 100, s);
    reconcileAlertLine("b,DISMISS,150,95,", 200, s);
    reconcileAlertLine("c,ONSET,210,95,", 300, s);
    reconcileAlertLine("d,DISMISS,150,95,", 400, s);
    reconcileAlertLine("e,ONSET,220,95,", 500, s);
    expect(s.open && s.onsetEpoch == 500, "the third episode is the open one");
  }
  // RESOLVED does not close, and junk does not crash
  {
    OpenEpisode s{};
    reconcileAlertLine("a,ONSET,200,95,", 100, s);
    reconcileAlertLine("b,RESOLVED,150,95,", 200, s);
    expect(s.open, "resolution does not close the episode - only dismissal does");
    reconcileAlertLine("", 300, s);
    reconcileAlertLine("timestamp,event,hr_bpm,spo2_pct,detail", 300, s);
    reconcileAlertLine("truncated", 300, s);
    expect(s.open, "a header, a blank line and a truncated line are ignored");
  }
}

static void testContacts() {
  {
    Contacts c{};
    parseContacts("224 432 969 / 931 / 970 / 973\n", c);
    expect(c.count == 1, "one line parses to one entry");
    expect(std::string(c.line[0]) == "224 432 969 / 931 / 970 / 973", "the line is verbatim");
  }
  {
    Contacts c{};
    parseContacts("first line\r\nsecond line\r\nthird line\r\n", c);
    expect(c.count == 2, "more than two lines keeps the first two");
    expect(std::string(c.line[1]) == "second line", "carriage returns are stripped");
  }
  {
    Contacts c{};
    parseContacts("012345678901234567890123456789EXTRA", c);
    expect(c.count == 1, "an over-long line is still one entry");
    expect(std::string(c.line[0]) == "012345678901234567890123456789",
           "an over-long line is truncated to thirty characters");
  }
  {
    Contacts c{};
    parseContacts("", c);
    expect(c.count == 0, "an empty file yields no lines");
    parseContacts(nullptr, c);
    expect(c.count == 0, "a missing file yields no lines");
  }
  {
    Contacts c{};
    parseContacts("\n\nreal line\n", c);
    expect(c.count == 1 && std::string(c.line[0]) == "real line", "blank lines are skipped");
  }
}

// Zero-based day of year, matching struct tm.tm_yday, for the 2026 dates used below.
static constexpr int YDAY_MAR20 = 78;
static constexpr int YDAY_MAR29 = 87;
static constexpr int YDAY_JUN21 = 171;
static constexpr int YDAY_SEP23 = 265;
static constexpr int YDAY_DEC21 = 354;

static constexpr int CET = 60;    // winter, UTC+1
static constexpr int CEST = 120;  // summer, UTC+2

static SolarTimes warsaw(int yday, int utcOffsetMin) {
  return solarTimesForDay(2026, yday, SITE_LATITUDE_DEG, SITE_LONGITUDE_DEG, utcOffsetMin);
}

static void expectNear(int actual, int expected, int tolerance, const char* message) {
  const int delta = actual > expected ? actual - expected : expected - actual;
  if (delta > tolerance) {
    std::fprintf(stderr, "FAIL: %s (got %02d:%02d, expected %02d:%02d)\n", message, actual / 60,
                 actual % 60, expected / 60, expected % 60);
    ++failures;
  }
}

static void testSolarTimes() {
  // Published Warsaw sunrise/sunset, to the minute. Three minutes of slack covers the difference
  // between the reference site's rounding and ours; the algorithm is good to well under that.
  {
    SolarTimes s = warsaw(YDAY_JUN21, CEST);
    expect(s.valid, "midsummer has both a sunrise and a sunset");
    expectNear(s.sunriseMin, 4 * 60 + 14, 3, "21 June sunrise is 04:14 CEST");
    expectNear(s.sunsetMin, 21 * 60 + 1, 3, "21 June sunset is 21:01 CEST");
  }
  {
    SolarTimes s = warsaw(YDAY_DEC21, CET);
    expectNear(s.sunriseMin, 7 * 60 + 44, 3, "21 December sunrise is 07:44 CET");
    expectNear(s.sunsetMin, 15 * 60 + 25, 3, "21 December sunset is 15:25 CET");
  }
  {
    SolarTimes s = warsaw(YDAY_MAR20, CET);
    expectNear(s.sunriseMin, 5 * 60 + 41, 3, "20 March sunrise is 05:41 CET");
    expectNear(s.sunsetMin, 17 * 60 + 49, 3, "20 March sunset is 17:49 CET");
  }
  {
    SolarTimes s = warsaw(YDAY_SEP23, CEST);
    expectNear(s.sunriseMin, 6 * 60 + 24, 3, "23 September sunrise is 06:24 CEST");
    expectNear(s.sunsetMin, 18 * 60 + 35, 3, "23 September sunset is 18:35 CEST");
  }

  // The clocks going forward must move the reported local times by exactly the offset change and
  // nothing else - this is the whole reason the offset is passed in rather than hardcoded.
  {
    SolarTimes winter = warsaw(YDAY_MAR29, CET);
    SolarTimes summer = warsaw(YDAY_MAR29, CEST);
    expect(summer.sunriseMin - winter.sunriseMin == 60, "DST shifts sunrise by exactly an hour");
    expect(summer.sunsetMin - winter.sunsetMin == 60, "DST shifts sunset by exactly an hour");
  }

  // Above the Arctic Circle in midsummer the sun never sets and the equation has no solution.
  {
    SolarTimes s = solarTimesForDay(2026, YDAY_JUN21, 78.2, 15.6, CEST);
    expect(!s.valid, "polar day yields no sunrise or sunset");
  }
}

// The offset is recovered from two breakdowns of one instant because the ESP32 has no tm_gmtoff.
// The cases that matter are the ones where local and UTC land on different dates.
static void testUtcOffsetRecovery() {
  auto at = [](int year, int yday, int hour, int min) {
    struct tm t{};
    t.tm_year = year - 1900;
    t.tm_yday = yday;
    t.tm_hour = hour;
    t.tm_min = min;
    return t;
  };

  // 21 June, 12:00 UTC is 14:00 CEST on the same day.
  expect(utcOffsetMinutes(at(2026, YDAY_JUN21, 14, 0), at(2026, YDAY_JUN21, 12, 0)) == CEST,
         "a same-day comparison gives the summer offset");
  // 21 December, 12:00 UTC is 13:00 CET.
  expect(utcOffsetMinutes(at(2026, YDAY_DEC21, 13, 0), at(2026, YDAY_DEC21, 12, 0)) == CET,
         "a same-day comparison gives the winter offset");
  // 23:30 UTC is 01:30 CEST the following morning - local is a day ahead.
  expect(utcOffsetMinutes(at(2026, YDAY_JUN21 + 1, 1, 30), at(2026, YDAY_JUN21, 23, 30)) == CEST,
         "local running a day ahead still gives a positive offset");
  // New Year's Eve in UTC, New Year's Day locally.
  expect(utcOffsetMinutes(at(2027, 0, 0, 0), at(2026, 364, 23, 0)) == CET,
         "a comparison across the year boundary gives the winter offset");
  // The mirror case: a western zone whose local date lags UTC.
  expect(utcOffsetMinutes(at(2026, YDAY_JUN21, 20, 0), at(2026, YDAY_JUN21 + 1, 1, 0)) == -300,
         "local running a day behind gives a negative offset");
}

static void testNightWindow() {
  // Midsummer: sunset 21:02 is past the latest permitted start, sunrise 04:14 is long before the
  // earliest permitted end. Both clamp, which is exactly the 04:14 wake-up this guards against.
  {
    NightWindow w = nightWindowFor(warsaw(YDAY_JUN21, CEST));
    expect(w.startMin == NIGHT_START_LATEST_MIN, "midsummer dimming is held back to 21:00");
    expect(w.endMin == NIGHT_END_EARLIEST_MIN, "midsummer brightening is held back to 07:00");
  }
  // Midwinter: sunset 15:25 would dim the screen mid-afternoon, so it clamps; sunrise 07:44 is
  // after the earliest permitted end, so the solar time is used unchanged.
  {
    NightWindow w = nightWindowFor(warsaw(YDAY_DEC21, CET));
    expect(w.startMin == NIGHT_START_EARLIEST_MIN, "midwinter dimming is held off until 19:00");
    expectNear(w.endMin, 7 * 60 + 44, 3, "midwinter brightening follows the late sunrise");
  }
  // A window that cannot be computed must still be usable and must not dim early.
  {
    SolarTimes invalid{};
    NightWindow w = nightWindowFor(invalid);
    expect(w.startMin == NIGHT_START_LATEST_MIN && w.endMin == NIGHT_END_EARLIEST_MIN,
           "an unusable solar result falls back to the conservative window");
  }
}

static void testNightWindowWrapsMidnight() {
  NightWindow w{21 * 60, 7 * 60};
  expect(!isNightAt(20 * 60 + 59, w), "a minute before the start is still day");
  expect(isNightAt(21 * 60, w), "the start minute is night");
  expect(isNightAt(23 * 60 + 59, w), "late evening is night");
  expect(isNightAt(0, w), "midnight is night");
  expect(isNightAt(3 * 60, w), "the small hours are night");
  expect(isNightAt(6 * 60 + 59, w), "a minute before the end is still night");
  expect(!isNightAt(7 * 60, w), "the end minute is day");
  expect(!isNightAt(12 * 60, w), "midday is day");
}

static void testBrightnessRamp() {
  NightWindow w{21 * 60, 7 * 60};

  expect(brightnessAt(20 * 60, w) == BRIGHT_DAY_LEVEL, "full brightness through the evening");
  expect(brightnessAt(21 * 60, w) == BRIGHT_DAY_LEVEL, "the fade begins at full brightness");
  expect(brightnessAt(21 * 60 + 60, w) == BRIGHT_NIGHT_LEVEL, "the fade reaches night level in an hour");
  expect(brightnessAt(23 * 60, w) == BRIGHT_NIGHT_LEVEL, "it stays at night level afterwards");
  expect(brightnessAt(3 * 60, w) == BRIGHT_NIGHT_LEVEL, "and through the small hours");

  const int half = brightnessAt(21 * 60 + 30, w);
  expect(half > BRIGHT_NIGHT_LEVEL && half < BRIGHT_DAY_LEVEL, "halfway down is between the two levels");
  expectNear(half, (BRIGHT_DAY_LEVEL + BRIGHT_NIGHT_LEVEL) / 2, 2, "halfway down is about halfway");

  // The evening ramp must never brighten, and the morning ramp must never dim. A single step in
  // the wrong direction would read as a flicker in a dark room.
  int previous = BRIGHT_DAY_LEVEL + 1;
  for (int t = 0; t <= 60; ++t) {
    const int level = brightnessAt((21 * 60 + t) % 1440, w);
    expect(level <= previous, "the evening ramp never brightens");
    previous = level;
  }

  expect(brightnessAt(7 * 60, w) == BRIGHT_NIGHT_LEVEL, "the morning fade begins at night level");
  expect(brightnessAt(8 * 60, w) == BRIGHT_DAY_LEVEL, "the morning fade reaches full in an hour");
  expect(brightnessAt(12 * 60, w) == BRIGHT_DAY_LEVEL, "midday is full brightness");

  previous = BRIGHT_NIGHT_LEVEL - 1;
  for (int t = 0; t <= 60; ++t) {
    const int level = brightnessAt(7 * 60 + t, w);
    expect(level >= previous, "the morning ramp never dims");
    previous = level;
  }

  // Every minute of the day has to land inside the configured range, wrap included.
  for (int m = 0; m < 1440; ++m) {
    const int level = brightnessAt(m, w);
    expect(level >= BRIGHT_NIGHT_LEVEL && level <= BRIGHT_DAY_LEVEL, "brightness stays in range");
  }
}

int main() {
  testSolarTimes();
  testUtcOffsetRecovery();
  testNightWindow();
  testNightWindowWrapsMidnight();
  testBrightnessRamp();
  testBandDecoder();
  testReadingMerge();
  testRadioHealth();
  testWifiFailover();
  testLiveRenderKey();
  testCriticalAlarm();
  testTraceWindow();
  testHoldGesture();
  testAlertLog();
  testContacts();
  if (failures) return EXIT_FAILURE;
  std::puts("firmware logic tests passed");
  return EXIT_SUCCESS;
}
