#include "../firmware/cyd_vitals/band_protocol.h"
#include "../firmware/cyd_vitals/critical_alarm.h"
#include "../firmware/cyd_vitals/trace_window.h"
#include "../firmware/cyd_vitals/hold_gesture.h"
#include "../firmware/cyd_vitals/alert_log.h"
#include "../firmware/cyd_vitals/contacts.h"
#include "../firmware/cyd_vitals/live_render_key.h"
#include "../firmware/cyd_vitals/vitals_csv.h"
#include "../firmware/cyd_vitals/radio_health.h"
#include "../firmware/cyd_vitals/wifi_failover.h"
#include "../firmware/cyd_vitals/day_night.h"
#include "../firmware/cyd_vitals/history.h"
#include "../firmware/cyd_vitals/day_bins.h"
#include "../firmware/cyd_vitals/gesture.h"
#include "../firmware/cyd_vitals/ota_password.h"

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

// The band reports a heart rate off bedding: 90/93/96 bpm against a mattress while its own beat
// interval implied 136/149/155. Worn, the two decodes agree. See docs/PROTOCOL.md.
static void testReadingDisagreement() {
  const uint8_t frame[23] = {
      0xF5, 0x03, 0x2D, 0x03, 0x04, 0x28, 0x01, 0x70,
      0x01, 0x70, 0x6C, 0x02, 0x30, 0x62, 0x03, 0x36,
      0x33, 0x05, 0x6C, 0x4E, 0xCD, 0xF7, 0xA4};
  BandReading reading{};
  expect(decodeBandFrame(frame, sizeof(frame), reading), "frame decodes");
  expect(reading.beatMs == 560, "beat interval is big-endian bytes 11-12");
  expect(!bandReadingDisagrees(reading.heartRate, reading.beatMs),
         "108 bpm against a 560 ms interval (107 bpm) agrees");

  // The three fabric measurements captured on 2026-08-26.
  expect(bandReadingDisagrees(90, 440), "90 bpm against 136 bpm implied is flagged");
  expect(bandReadingDisagrees(93, 404), "93 bpm against 149 bpm implied is flagged");
  expect(bandReadingDisagrees(96, 387), "96 bpm against 155 bpm implied is flagged");

  // Settled on an adult hand the same day, and the worst genuine infant disagreement seen.
  expect(!bandReadingDisagrees(69, 840), "69 bpm against 71 bpm implied is accepted");
  expect(!bandReadingDisagrees(68, 815), "68 bpm against 74 bpm implied is accepted");
  expect(!bandReadingDisagrees(120, 462), "a 10 bpm spread stays inside the band");

  expect(!bandReadingDisagrees(0, 440), "an idle band with no heart rate is not an issue");
  expect(!bandReadingDisagrees(96, 0), "a missing interval cannot contradict anything");
  expect(!bandReadingDisagrees(0, 0), "the charging beacon is not an issue");
}

static void testReadingMerge() {
  RateContext context{};
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

  ReadingSnapshot next = mergeBandReading(previous, frame, -70, 12'345, context);
  expect(next.sequence == 45 && next.heartRate == 108, "new vitals are published together");
  expect(next.oxygenSaturation == 98 && next.signal == 4,
         "oxygen and signal belong to the same packet");
  expect(next.skinValid && std::fabs(next.skinC - 36.8f) < 0.01f, "invalid skin preserves held value");
  expect(next.rssi == -70 && next.lastPacketMs == 12'345, "metadata belongs to the same packet");
  expect(next.effectiveHeartRate == 108 && !next.heartRateCorrected,
         "an agreeing reading is shown as it is");
  expect(next.highAlarmHeartRate == 108, "and is its own alarm input");

  frame.skinC = 37.2f;
  frame.skinValid = true;
  ReadingSnapshot warm = mergeBandReading(next, frame, -68, 12'500, context);
  expect(warm.skinValid && std::fabs(warm.skinC - 37.2f) < 0.01f,
         "valid skin replaces the held value");
  expect(context.count == 1, "a rebroadcast of the same measurement is noted once");

  // Agreeing measurements build the context. After three of them a halved byte is corrected, a
  // doubled interval is ignored, and the decision holds across rebroadcasts of the same sequence.
  RateContext ctx{};
  ReadingSnapshot snap{};
  BandReading m{};
  m.heartRate = 160;
  m.beatMs = 375;
  uint32_t t = 0;
  for (uint8_t seq = 1; seq <= 3; ++seq) {
    m.sequence = seq;
    snap = mergeBandReading(snap, m, -60, t, ctx);
    t += 20'000;
  }
  expect(ctx.count == 3, "three measurements are three context entries");
  m.sequence = 4;
  m.heartRate = 80;                       // the byte halves while the interval holds at 375 ms
  snap = mergeBandReading(snap, m, -60, t, ctx);
  expect(snap.effectiveHeartRate == 160 && snap.heartRateCorrected, "a halved byte is corrected");
  expect(snap.highAlarmHeartRate == 160, "and the alarm hears the same rate");
  expect(ctx.count == 3, "a disagreeing measurement does not enter the context");
  snap = mergeBandReading(snap, m, -61, t + 1'500, ctx);
  expect(snap.effectiveHeartRate == 160 && snap.heartRateCorrected,
         "a rebroadcast keeps the decision");
  t += 20'000;
  m.sequence = 5;
  m.heartRate = 160;
  m.beatMs = 750;                         // now the interval doubles while the byte holds
  snap = mergeBandReading(snap, m, -60, t, ctx);
  expect(snap.effectiveHeartRate == 160 && !snap.heartRateCorrected,
         "a doubled interval is ignored");
  expect(snap.highAlarmHeartRate == 160, "and does not feed the alarm");

  // Before any context exists the byte stands even when the interval contradicts it - but the
  // alarm still hears the faster decode.
  RateContext empty{};
  ReadingSnapshot fresh{};
  m.sequence = 9;
  m.heartRate = 80;
  m.beatMs = 375;
  fresh = mergeBandReading(fresh, m, -60, 0, empty);
  expect(fresh.effectiveHeartRate == 80 && !fresh.heartRateCorrected, "no context keeps the byte");
  expect(fresh.highAlarmHeartRate == 160, "but the alarm still hears the faster decode");
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
  second.readingIssue = !second.readingIssue;
  expect(first != second, "a doubtful reading redraws the heart cell");
  second = first;
  second.minuteKey++;
  expect(first != second, "new minute redraws clock");
  second = first;
  second.intervalHeartRate++;
  expect(first != second, "a changed interval rate redraws the heart cell");

  // When the byte is kept despite a disagreeing interval, the cell says so and carries the
  // interval's own rate for the note. A corrected reading is not also an issue. An unusable
  // interval is an issue with no rate to show.
  ReadingSnapshot kept{};
  kept.sequence = 46;
  kept.heartRate = 126;
  kept.beatMs = 880;
  kept.effectiveHeartRate = 126;
  kept.highAlarmHeartRate = 126;
  LiveRenderKey keptKey = makeLiveRenderKey(kept, RadioState::RECEIVING, false, ALERT_NONE, 610);
  expect(keptKey.readingIssue && !keptKey.corrected, "a kept byte under a doubled interval is an issue");
  expect(keptKey.intervalHeartRate == 68, "the interval's own rate is carried for the note");

  ReadingSnapshot corrected{};
  corrected.heartRate = 83;
  corrected.beatMs = 352;
  corrected.effectiveHeartRate = 170;
  corrected.heartRateCorrected = true;
  corrected.highAlarmHeartRate = 170;
  LiveRenderKey ck = makeLiveRenderKey(corrected, RadioState::RECEIVING, false, ALERT_NONE, 610);
  expect(ck.corrected && !ck.readingIssue, "a corrected reading is not also an issue");
  expect(ck.intervalHeartRate == 170, "and carries the interval rate it shows");

  ReadingSnapshot garbage{};
  garbage.heartRate = 200;
  garbage.beatMs = 50;
  garbage.effectiveHeartRate = 200;
  garbage.highAlarmHeartRate = 200;
  LiveRenderKey gk = makeLiveRenderKey(garbage, RadioState::RECEIVING, false, ALERT_NONE, 610);
  expect(gk.readingIssue && gk.intervalHeartRate == 0, "an unusable interval is an issue with no rate");
}

// Feed readings 20 s apart while ticking every second, exactly as the main loop does, and run
// long enough for at least one confirmation window to close.
// A reading plus whether it came from the beat interval, so a window can mix the two.
struct WindowReading {
  int hr;
  bool corrected;
};

static AlarmMachine runWindow(const AlarmConfig& cfg, const std::vector<WindowReading>& readings) {
  AlarmMachine m{};
  const uint32_t epoch0 = 1'000'000;
  uint32_t endMs = static_cast<uint32_t>(readings.size()) * 20'000;
  // A corrected window is twice as long, so the harness has to outlast it - but only when the
  // readings actually are corrected. Running a raw window past its 60 s would hand it a second
  // confirmation window it would never get in the loop, and quietly change what these tests mean.
  bool anyCorrected = false;
  for (const WindowReading& r : readings) anyCorrected = anyCorrected || r.corrected;
  const uint32_t minEnd = anyCorrected ? cfg.confirmMsCorrected : cfg.confirmMs;
  if (endMs < minEnd) endMs = minEnd;
  endMs += 1'000;
  for (uint32_t t = 0; t <= endMs; t += 1'000) {
    const size_t idx = t / 20'000;
    if (t % 20'000 == 0 && idx < readings.size()) {
      alarmOnReading(m, cfg, readings[idx].hr, t, epoch0 + t / 1000, readings[idx].corrected);
    }
    alarmTick(m, cfg, t, false);
  }
  return m;
}

static AlarmMachine runWindow(const AlarmConfig& cfg, const std::vector<int>& readings) {
  std::vector<WindowReading> raw;
  raw.reserve(readings.size());
  for (int hr : readings) raw.push_back({hr, false});
  return runWindow(cfg, raw);
}

// The same machine with its thresholds read the other way up: arms at or below 80, confirms at
// or below 90, abandons above 100. Mirrors testCriticalAlarm() case for case.
static AlarmConfig lowConfig() {
  AlarmConfig cfg{};
  cfg.crit = 80;
  cfg.sustain = 90;
  cfg.cancel = 100;
  cfg.lowSide = true;
  return cfg;
}

static void testLowAlarm() {
  const AlarmConfig cfg = lowConfig();

  // A zero heart rate is not a slow one. An idle or charging band broadcasts it every ~2 s, and
  // it must never arm, seed a window, or count as a recovery.
  {
    AlarmMachine m{};
    for (int i = 0; i < 20; ++i) alarmOnReading(m, cfg, 0, i * 20'000u, 1'000'000 + i * 20);
    expect(m.state == AlarmState::IDLE, "a charging band never arms the low alarm");
    expect(!m.haveLast, "a zero reading is not remembered as a reading");
    AlarmMachine idle = runWindow(cfg, {0, 0, 0, 0});
    expect(idle.state == AlarmState::IDLE, "a window of zeros does not alarm");
  }

  // 1: one qualifying reading arms but does not alarm
  {
    AlarmMachine m{};
    alarmOnReading(m, cfg, 75, 0, 1'000'000);
    expect(m.state == AlarmState::ARMED, "one reading at 75 arms");
    alarmTick(m, cfg, 5'000, false);
    expect(m.state == AlarmState::ARMED, "still armed before the window closes");
    expect(m.peak == 75, "the episode extreme starts at the arming reading");
  }
  {
    AlarmMachine m{};
    alarmOnReading(m, cfg, 80, 0, 1'000'000);
    expect(m.state == AlarmState::ARMED, "the threshold itself is inclusive");
    AlarmMachine n{};
    alarmOnReading(n, cfg, 81, 0, 1'000'000);
    expect(n.state == AlarmState::IDLE, "81 is inside the safe band");
  }

  // 2: sustained readings alarm, onset is the FIRST reading
  {
    AlarmMachine m = runWindow(cfg, {75, 74, 73, 72});
    expect(m.state == AlarmState::ALARM, "a sustained slow rate alarms");
    expect(m.cause == AlarmCause::CONFIRMED_LOW, "cause is CONFIRMED_LOW");
    expect(m.onsetEpoch == 1'000'000, "onset is the first qualifying reading");
    expect(m.peak == 72, "the extreme of a low episode is its slowest reading");
  }

  // 3: a reading past cancel returns to IDLE
  {
    AlarmMachine m{};
    alarmOnReading(m, cfg, 75, 0, 1'000'000);
    alarmOnReading(m, cfg, 101, 20'000, 1'000'020);
    expect(m.state == AlarmState::IDLE, "101 cancels the window");
  }

  // 4: staleness while armed escalates - silence over a slow heart is not a recovery
  {
    AlarmMachine m{};
    alarmOnReading(m, cfg, 75, 0, 1'000'000);
    AlarmEvent e = alarmTick(m, cfg, 30'000, true);
    expect(m.state == AlarmState::ALARM, "stale while armed alarms");
    expect(m.cause == AlarmCause::LOST_WHILE_CRITICAL, "cause is LOST_WHILE_CRITICAL");
    expect(e.alarmStarted, "the transition is reported once");
    expect(!alarmTick(m, cfg, 31'000, true).alarmStarted, "repeated stale ticks do not re-fire");
  }

  // 5: recovery resolves the latched episode but does not clear it
  {
    AlarmMachine m = runWindow(cfg, {75, 74, 73, 72});
    AlarmEvent e = alarmOnReading(m, cfg, 120, 200'000, 1'000'200);
    expect(e.episodeResolved, "climbing back above the threshold resolves the episode");
    expect(m.state == AlarmState::ALARM, "the alarm still latches until it is dismissed");
    expect(!alarmOnReading(m, cfg, 125, 220'000, 1'000'220).episodeResolved,
           "resolution is reported once");
  }

  // 6: the collapse rule stays high-side only
  {
    AlarmMachine m{};
    alarmOnReading(m, cfg, 55, 0, 1'000'000);
    alarmOnReading(m, cfg, 175, 20'000, 1'000'020);
    expect(m.cause != AlarmCause::IMPLAUSIBLE_COLLAPSE,
           "a jump from slow to fast is an artifact, not a collapse");
  }

  // 7: the window table, mirrored. Recovery means climbing.
  struct LowCase { std::vector<int> readings; bool alarms; const char* why; };
  const std::vector<LowCase> cases = {
    {{75, 85, 95},  false, "climbing and ends above sustain"},
    {{75, 95, 85},  true,  "fell again, ends below sustain"},
    {{75, 85, 85},  true,  "flat is not recovering"},
    {{75, 85, 84},  true,  "a one-beat fall is still not recovering"},
    {{75, 82, 87},  false, "strictly climbing with one critical reading"},
    {{75, 80, 85},  true,  "count path fires despite climbing"},
    {{75, 95, 92},  false, "climbed but ends above sustain"},
    {{75, 79, 75},  true,  "two critical readings, ends low"},
    {{75, 75},      true,  "count is absolute, not a fraction"},
    {{75, 80, 90},  true,  "sustain boundary is inclusive"},
  };
  for (const LowCase& c : cases) {
    AlarmMachine m = runWindow(cfg, c.readings);
    const bool alarmed = m.state == AlarmState::ALARM;
    expect(alarmed == c.alarms, c.why);
  }

  // 8: a dismissal snoozes the low alarm exactly as it does the high one
  {
    AlarmMachine m = runWindow(cfg, {75, 74, 73, 72});
    alarmDismiss(m, 200'000);
    expect(m.state == AlarmState::IDLE, "dismissal clears the latch");
    AlarmMachine again = m;
    for (uint32_t t = 200'000; t <= 340'000; t += 1'000) {
      if (t % 20'000 == 0) alarmOnReading(again, cfg, 70, t, 1'000'000 + t / 1000);
      alarmTick(again, cfg, t, false);
    }
    expect(again.state != AlarmState::ALARM, "a fresh episode inside the snooze stays silent");
  }
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
    expect(!s.low, "a high onset is not marked low");
    OpenEpisode slow{};
    // Resumed after a power cut, the screen has to say which way the heart rate went.
    reconcileAlertLine("2026-08-10 03:14:22,ONSET,68,94,CONFIRMED_LOW", 1'000'000, slow);
    expect(slow.open && slow.low, "a low onset is remembered as low");
    expect(s.open && s.onsetEpoch == 1'000'000, "an unmatched onset is open");
  }
  // a DISMISS closes it
  {
    OpenEpisode s{};
    reconcileAlertLine("2026-08-10 03:14:22,ONSET,214,94,CONFIRMED_HIGH", 1'000'000, s);
    reconcileAlertLine("2026-08-10 03:20:58,DISMISS,171,93,peak=221", 1'000'396, s);
    expect(!s.low, "a dismissed episode carries no side");
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

// Both of the band's decodes lock onto every second beat now and then, independently. Whichever
// decode is continuous with the last five minutes of agreeing readings is the one to show; the
// context below is what a disagreement is judged against. See docs/PROTOCOL.md.
static void testRateContext() {
  RateContext c{};
  int ref = 0;
  expect(!rateContextReference(c, 0, ref), "an empty context offers no reference");
  rateContextNote(c, 120, 0);
  rateContextNote(c, 124, 20'000);
  expect(!rateContextReference(c, 40'000, ref), "two readings are not a reference");
  rateContextNote(c, 122, 40'000);
  expect(rateContextReference(c, 60'000, ref) && ref == 122, "three readings give their median");
  rateContextNote(c, 160, 60'000);
  expect(rateContextReference(c, 80'000, ref) && ref == 123, "an even count averages the middle pair");

  // Entries older than five minutes fall out of the reference.
  expect(!rateContextReference(c, 360'001, ref), "readings older than 300 s expire");
  rateContextNote(c, 130, 360'000);
  rateContextNote(c, 132, 380'000);
  rateContextNote(c, 134, 400'000);
  expect(rateContextReference(c, 400'000, ref) && ref == 132, "only the recent readings count");

  // The ring keeps the newest RATE_CONTEXT_CAP readings and no more.
  RateContext ring{};
  for (int i = 0; i < RATE_CONTEXT_CAP + 4; ++i) rateContextNote(ring, 200 + i, 500'000 + i * 1'000);
  expect(ring.count == RATE_CONTEXT_CAP, "the ring is bounded");
  expect(rateContextReference(ring, 520'000, ref) && ref == 200 + 4 + (RATE_CONTEXT_CAP - 1) / 2,
         "the oldest readings are the ones dropped");
}

static void testResolveHeartRate() {
  // Agreement leaves the byte alone, with or without context.
  expect(resolveHeartRate(108, 560, true, 110) == 108, "an agreeing reading is the byte");
  expect(resolveHeartRate(108, 560, false, 0) == 108, "with no context too");

  // 2026-09-19 12:59:42: the byte fell from 171 to 83 while the interval stayed at 352 ms.
  expect(resolveHeartRate(83, 352, true, 168) == 170, "a halved byte gives way to the interval");
  // 2026-09-19 06:42:34: the interval doubled to 880 ms while the byte stayed at 126.
  expect(resolveHeartRate(126, 880, true, 127) == 126, "a doubled interval gives way to the byte");
  // 2026-09-16 20:07:35: byte 90 against 291 ms (206 bpm) with the last five minutes at 166.
  expect(resolveHeartRate(90, 291, true, 166) == 206, "the candidate nearer the recent rate wins");
  // 2026-09-09 13:49:18: byte 117 against 329 ms (182 bpm) with the last five minutes at 125.
  expect(resolveHeartRate(117, 329, true, 125) == 117, "whichever decode that turns out to be");

  // Without three recent agreeing readings there is nothing to compare against: the byte stands.
  expect(resolveHeartRate(83, 352, false, 0) == 83, "no context keeps the byte");
  // A tie goes to the byte, the decode one step closer to the sensor.
  expect(resolveHeartRate(100, 300, true, 150) == 100, "a tie keeps the byte");

  // Garbage intervals never take over.
  expect(resolveHeartRate(200, 50, true, 120) == 200, "a 50 ms interval (1200 bpm) is rejected");
  expect(resolveHeartRate(200, 149, true, 120) == 200, "just under 150 ms is rejected");
  expect(resolveHeartRate(120, 2001, true, 120) == 120, "just over 2000 ms is rejected");
  expect(resolveHeartRate(200, 0, true, 120) == 200, "a missing interval cannot correct anything");
  expect(resolveHeartRate(100, 150, true, 400) == 400, "150 ms is accepted and reads 400 bpm");
  expect(resolveHeartRate(100, 2000, true, 30) == 30, "2000 ms is accepted and reads 30 bpm");

  // A band with no pulse must never be handed a manufactured rate.
  expect(resolveHeartRate(0, 375, true, 160) == 0, "an idle band stays at zero");

  // Rounding is to nearest, not toward zero: 60000/271 is 221.4.
  expect(resolveHeartRate(110, 271, true, 220) == 221, "the interval rounds to nearest");
  expect(intervalHeartRate(271) == 221, "intervalHeartRate rounds the same way");
  expect(intervalHeartRate(149) == 0 && intervalHeartRate(2001) == 0 && intervalHeartRate(0) == 0,
         "an unusable interval has no rate");
}

// The high alarm hears the faster decode whenever the band contradicts itself, so a sudden
// tachycardia whose byte arrives halved is still counted even while the screen keeps the byte.
static void testHighAlarmHeartRate() {
  // Onset: the last five minutes sat at 130, the byte halves to 115, the interval says 270 ms.
  const int shown = resolveHeartRate(115, 270, true, 130);
  expect(shown == 115, "continuity keeps the byte at onset");
  expect(highAlarmHeartRate(115, 270, shown) == 222, "the alarm still hears 222");
  // A doubled interval never hands the alarm a slower rate than the screen shows.
  expect(highAlarmHeartRate(126, 880, 126) == 126, "a slow interval is not an alarm input");
  // Agreement, garbage and silence pass the shown rate through.
  expect(highAlarmHeartRate(108, 560, 108) == 108, "an agreeing reading is unchanged");
  expect(highAlarmHeartRate(200, 50, 200) == 200, "a garbage interval is ignored");
  expect(highAlarmHeartRate(0, 375, 0) == 0, "an idle band stays at zero");
  // When the screen already shows the interval, the two are the same number.
  expect(highAlarmHeartRate(83, 352, 170) == 170, "a corrected reading is its own alarm input");
}

// The scenario the separate alarm input exists for, and the glitch it must shrug off.
static void testHalvedOnsetStillAlarms() {
  AlarmConfig cfg{};
  const int shown = resolveHeartRate(115, 270, true, 130);     // 115 stays on the screen
  const int heard = highAlarmHeartRate(115, 270, shown);        // 222 reaches the high alarm
  AlarmMachine high = runWindow(cfg, std::vector<WindowReading>{
      {heard, true}, {heard, true}, {heard, true}, {heard, true},
      {heard, true}, {heard, true}, {heard, true}});
  expect(high.state == AlarmState::ALARM, "a halved onset still confirms");
  expect(high.cause == AlarmCause::CONFIRMED_HIGH_BEAT, "and is named beat-derived");
  AlarmMachine low = runWindow(lowConfig(), std::vector<WindowReading>{
      {shown, false}, {shown, false}, {shown, false}, {shown, false}});
  expect(low.state == AlarmState::IDLE, "115 on the screen never arms the low alarm");

  // The band's fifteen-minute glitch: one reading of 280 ms (214 bpm) between ordinary ones
  // arms the window and the next ordinary reading abandons it.
  AlarmMachine glitch{};
  alarmOnReading(glitch, cfg, highAlarmHeartRate(110, 545, 110), 0, 1'000'000, false);
  alarmOnReading(glitch, cfg, highAlarmHeartRate(107, 280, 107), 20'000, 1'000'020, true);
  expect(glitch.state == AlarmState::ARMED, "one glitch arms the window");
  alarmOnReading(glitch, cfg, highAlarmHeartRate(112, 540, 112), 40'000, 1'000'040, false);
  expect(glitch.state == AlarmState::IDLE, "the next ordinary reading abandons it");
}

// A window holding any beat-derived reading needs three critical readings across 120 s instead of
// two across 60 s. Same thresholds, same latch: only the burden of proof moves.
static void testCorrectedWindow() {
  AlarmConfig cfg{};

  {
    // Six corrected readings at or above crit, held there: confirmed, and named as beat-derived.
    AlarmMachine m = runWindow(cfg, std::vector<WindowReading>{
        {206, true}, {205, true}, {205, true}, {205, true}, {205, true}, {205, true}});
    expect(m.state == AlarmState::ALARM, "a sustained corrected rate alarms");
    expect(m.cause == AlarmCause::CONFIRMED_HIGH_BEAT, "the cause names the beat interval");
    expect(m.windowCorrected, "the window is marked corrected");
  }

  {
    // Two corrected readings inside the first 60 s are not enough, where two raw ones would be.
    AlarmMachine m{};
    alarmOnReading(m, cfg, 205, 0, 1'000'000, true);
    alarmOnReading(m, cfg, 205, 20'000, 1'000'020, true);
    alarmTick(m, cfg, 61'000, false);
    expect(m.state == AlarmState::ARMED, "two corrected readings do not confirm at 60 s");
    AlarmMachine raw{};
    alarmOnReading(raw, cfg, 205, 0, 1'000'000);
    alarmOnReading(raw, cfg, 205, 20'000, 1'000'020);
    alarmTick(raw, cfg, 61'000, false);
    expect(raw.state == AlarmState::ALARM, "two raw readings still confirm at 60 s");
    expect(raw.cause == AlarmCause::CONFIRMED_HIGH, "a raw window keeps the plain cause");
  }

  {
    // A rate falling strictly back through the window is resolving, and is not confirmed.
    AlarmMachine m = runWindow(cfg, std::vector<WindowReading>{
        {206, true}, {205, true}, {199, true}, {198, true}, {197, true}, {196, true}});
    expect(m.state == AlarmState::ARMED, "a strictly falling corrected rate does not alarm");
  }

  {
    // One corrected reading is enough to widen a window that is otherwise raw.
    AlarmMachine m{};
    alarmOnReading(m, cfg, 205, 0, 1'000'000, false);
    alarmOnReading(m, cfg, 205, 20'000, 1'000'020, true);
    alarmTick(m, cfg, 61'000, false);
    expect(m.state == AlarmState::ARMED, "one corrected reading widens the window to 120 s");
    alarmOnReading(m, cfg, 205, 80'000, 1'000'080, true);
    alarmTick(m, cfg, 121'000, false);
    expect(m.state == AlarmState::ALARM, "three readings across 120 s confirm");
    expect(m.cause == AlarmCause::CONFIRMED_HIGH_BEAT, "a mixed window is named beat-derived");
  }

  {
    // Cancel still works, and a raw reading below cancel ends a corrected window outright.
    AlarmMachine m{};
    alarmOnReading(m, cfg, 205, 0, 1'000'000, true);
    alarmOnReading(m, cfg, 205, 20'000, 1'000'020, true);
    alarmOnReading(m, cfg, 130, 40'000, 1'000'040, false);
    expect(m.state == AlarmState::IDLE, "a reading below cancel abandons a corrected window");
    expect(!m.windowCorrected, "an abandoned window forgets it was corrected");
  }

  {
    // The collapse rule must not fire on a corrected reading: one long interval would otherwise
    // turn a normal rate into an instant alarm with no window to catch it.
    AlarmMachine m{};
    alarmOnReading(m, cfg, 190, 0, 1'000'000, false);
    alarmOnReading(m, cfg, 40, 20'000, 1'000'020, true);
    expect(m.state == AlarmState::IDLE, "a corrected reading cannot fire the collapse rule");
    AlarmMachine rawCollapse{};
    alarmOnReading(rawCollapse, cfg, 190, 0, 1'000'000, false);
    alarmOnReading(rawCollapse, cfg, 40, 20'000, 1'000'020, false);
    expect(rawCollapse.state == AlarmState::ALARM, "a raw collapse still alarms");
    expect(rawCollapse.cause == AlarmCause::IMPLAUSIBLE_COLLAPSE, "and is named a collapse");
  }

  {
    // Dismissal snoozes a corrected episode exactly as it snoozes a raw one.
    AlarmMachine m = runWindow(cfg, std::vector<WindowReading>{
        {206, true}, {205, true}, {205, true}, {205, true}, {205, true}, {205, true}});
    expect(m.state == AlarmState::ALARM, "the corrected episode alarmed");
    alarmDismiss(m, 200'000);
    expect(m.state == AlarmState::IDLE, "dismissal clears it");
    for (uint32_t t = 220'000; t <= 400'000; t += 20'000) {
      alarmOnReading(m, cfg, 205, t, 1'000'000 + t / 1000, true);
      alarmTick(m, cfg, t, false);
    }
    expect(m.state != AlarmState::ALARM, "the snooze holds a corrected alarm too");
  }

  // The halved reading that used to trip the bradycardia alarm no longer reaches it.
  {
    AlarmConfig low = lowConfig();
    const int corrected = resolveHeartRate(80, 375, true, 160);
    AlarmMachine m = runWindow(low, std::vector<WindowReading>{
        {corrected, true}, {corrected, true}, {corrected, true}, {corrected, true}});
    expect(m.state == AlarmState::IDLE, "a halved 80 corrected to 160 never arms the low alarm");
    // A genuinely slow heart still does: both decodes agree, so nothing is corrected.
    AlarmMachine real = runWindow(low, std::vector<int>{78, 76, 75, 74, 74, 74});
    expect(real.state == AlarmState::ALARM, "a real bradycardia still alarms");
    expect(real.cause == AlarmCause::CONFIRMED_LOW, "and is named a low episode");
  }
}

// The daily log gained beat_ms and hr_eff. The parser has to read the rows written before they
// existed, including the mixed file that upgrade day leaves behind.
static void testVitalsCsv() {
  char row[80];
  formatVitalsRow(row, sizeof(row), "2026-09-02 15:59:38", 143, 96, 35.0f, true, 375, 160);
  expect(std::string(row) == "2026-09-02 15:59:38,143,96,35.0,375,160", "a full row formats");
  formatVitalsRow(row, sizeof(row), "2026-09-02 15:59:38", 143, 96, 0.0f, false, 0, 143);
  expect(std::string(row) == "2026-09-02 15:59:38,143,96,,0,143", "an invalid skin leaves a hole");

  VitalsRow parsed{};
  expect(parseVitalsRow("2026-09-02 15:59:38,143,96,35.0,375,160", parsed), "a full row parses");
  expect(parsed.hh == 15 && parsed.mm == 59 && parsed.ss == 38, "the clock is read by offset");
  expect(parsed.hr == 143 && parsed.spo2 == 96, "raw rate and oxygen");
  expect(parsed.skinValid && std::fabs(parsed.skinC - 35.0f) < 0.01f, "skin stops at the comma");
  expect(parsed.beatMs == 375 && parsed.hrEff == 160, "beat interval and corrected rate");
  expect(parsed.haveEff, "the row carries a corrected rate");
  expect(vitalsRowHistoryHr(parsed) == 160, "the charts plot the corrected rate");

  VitalsRow old{};
  expect(parseVitalsRow("2026-08-07 15:59:38,134,96,35.0", old), "a four-column row parses");
  expect(old.hr == 134 && old.skinValid && std::fabs(old.skinC - 35.0f) < 0.01f,
         "the old row's skin is not swallowed");
  expect(!old.haveEff, "an old row has no corrected rate");
  expect(vitalsRowHistoryHr(old) == 134, "and is plotted from the raw byte");

  VitalsRow noSkin{};
  expect(parseVitalsRow("2026-08-07 15:59:21,134,96,", noSkin), "an empty skin column parses");
  expect(!noSkin.skinValid, "and is not mistaken for a temperature");

  VitalsRow emptySkinWide{};
  expect(parseVitalsRow("2026-09-02 16:00:00,110,97,,271,221", emptySkinWide),
         "an empty skin column in a wide row parses");
  expect(!emptySkinWide.skinValid, "still no temperature");
  expect(emptySkinWide.beatMs == 271 && emptySkinWide.hrEff == 221, "the later columns still land");

  VitalsRow bad{};
  expect(!parseVitalsRow("timestamp,hr_bpm,spo2_pct,skin_c,beat_ms,hr_eff", bad),
         "the header is not a reading");
  expect(!parseVitalsRow("", bad), "an empty line is not a reading");
  expect(!parseVitalsRow("2026-09-02 15:59", bad), "a truncated line is not a reading");
}

static void testHistory() {
  History h{};
  expect(h.count == 0 && historyNewestEpoch(h) == 0, "an empty history has no newest epoch");

  historyPush(h, 120, 97, 1'000'000);
  historyPush(h, 125, 98, 1'000'020);
  historyPush(h, 130, 0, 1'000'040);
  expect(h.count == 3, "three pushes are three samples");
  expect(h.heartRate[historyOldest(h, 0)] == 120, "oldest(0) is the first sample");
  expect(h.heartRate[historyOldest(h, 2)] == 130, "oldest(count-1) is the last sample");
  expect(h.heartRate[historyNewest(h, 0)] == 130, "newest(0) is the last sample");
  expect(h.heartRate[historyNewest(h, 2)] == 120, "newest(count-1) is the first sample");
  expect(historyNewestEpoch(h) == 1'000'040, "the newest epoch is the largest recorded");

  // Values are stored in a byte and clamped rather than wrapped: 300 must not become 44.
  historyPush(h, 300, -5, 0);
  expect(h.heartRate[historyNewest(h, 0)] == 254, "a value above the byte range is clamped");
  expect(h.oxygen[historyNewest(h, 0)] == 0, "a negative value is clamped to zero");
  expect(tracePositional(h.epoch, h.count), "a sample without a timestamp forces the positional axis");

  // The ring keeps the newest HISTORY_LEN samples, and the two index helpers agree across the wrap.
  History ring{};
  for (int i = 0; i < HISTORY_LEN + 10; ++i) historyPush(ring, i % 200, 95, 2'000'000 + i);
  expect(ring.count == HISTORY_LEN, "the ring is bounded");
  expect(ring.epoch[historyOldest(ring, 0)] == 2'000'010, "the oldest samples are the ones dropped");
  expect(ring.epoch[historyNewest(ring, 0)] == 2'000'000 + HISTORY_LEN + 9, "the newest is the last pushed");
  bool consistent = true;
  for (int i = 0; i < ring.count; ++i) {
    if (historyOldest(ring, i) != historyNewest(ring, ring.count - 1 - i)) consistent = false;
  }
  expect(consistent, "oldest(i) and newest(count-1-i) name the same slot");
  expect(historyNewestEpoch(ring) == 2'000'000 + HISTORY_LEN + 9, "newest epoch survives the wrap");
}

static void testDayBins() {
  DayBins d{};
  const BinStats none = dayBinsStats(d, DayMetric::HEART_RATE, 0, DAY_BIN_COUNT);
  expect(none.count == 0 && none.mean == 0.0f && none.sd == 0.0f, "empty bins have no statistics");

  // Three readings in the 10:00 bin: mean 120, population sd sqrt(200/3).
  dayBinsAdd(d, 110, 97, 10 * 60 + 1);
  dayBinsAdd(d, 120, 98, 10 * 60 + 7);
  dayBinsAdd(d, 130, 99, 10 * 60 + 14);
  const int tenOclock = (10 * 60) / DAY_BIN_MINUTES;
  BinStats st = dayBinsStats(d, DayMetric::HEART_RATE, tenOclock, 1);
  expect(st.count == 3, "three readings land in the 10:00 bin");
  expect(std::fabs(st.mean - 120.0f) < 0.01f, "the mean is 120");
  expect(std::fabs(st.sd - std::sqrt(200.0f / 3.0f)) < 0.01f, "the sd is the population sd");
  BinStats ox = dayBinsStats(d, DayMetric::OXYGEN, tenOclock, 1);
  expect(ox.count == 3 && std::fabs(ox.mean - 98.0f) < 0.01f, "oxygen is binned alongside");

  // A zero is "no reading" and is not averaged in; a constant series has an sd of exactly zero.
  dayBinsAdd(d, 0, 0, 10 * 60 + 2);
  expect(dayBinsStats(d, DayMetric::HEART_RATE, tenOclock, 1).count == 3, "a zero reading is not counted");
  DayBins flat{};
  for (int i = 0; i < 40; ++i) dayBinsAdd(flat, 133, 96, 12 * 60);
  expect(dayBinsStats(flat, DayMetric::HEART_RATE, (12 * 60) / DAY_BIN_MINUTES, 1).sd == 0.0f,
         "a constant series has zero spread, not a rounding artefact");

  // Grouping: the 10:15 bin joins the 10:00 bin in a one-hour group.
  dayBinsAdd(d, 200, 90, 10 * 60 + 20);
  const BinStats hour = dayBinsStats(d, DayMetric::HEART_RATE, tenOclock, 4);
  expect(hour.count == 4 && std::fabs(hour.mean - 140.0f) < 0.01f, "adjacent bins group into an hour");

  // Out-of-range minutes are ignored rather than written past the array.
  dayBinsAdd(d, 150, 95, -1);
  dayBinsAdd(d, 150, 95, 1440);
  dayBinsAdd(d, 150, 95, 99999);
  expect(dayBinsStats(d, DayMetric::HEART_RATE, 0, DAY_BIN_COUNT).count == 4, "out-of-range minutes are dropped");
}

static void testGesture() {
  const int SWIPE = 130;
  const int MOVE = 20;
  const uint32_t TAP_MS = 600;

  // A quick touch that does not move is a tap, reported at its starting point on lift-off.
  {
    GestureTracker g{};
    GestureResult r = gestureUpdate(g, true, 100, 120, 0, SWIPE, MOVE, TAP_MS);
    expect(r.gesture == Gesture::NONE, "nothing resolves while the finger is down");
    r = gestureUpdate(g, true, 105, 118, 100, SWIPE, MOVE, TAP_MS);
    expect(r.gesture == Gesture::NONE, "still nothing while jittering under the finger");
    r = gestureUpdate(g, false, 0, 0, 200, SWIPE, MOVE, TAP_MS);
    expect(r.gesture == Gesture::TAP, "lift-off resolves a tap");
    expect(r.tapX == 100 && r.tapY == 120, "the tap is reported where it began");
    r = gestureUpdate(g, false, 0, 0, 300, SWIPE, MOVE, TAP_MS);
    expect(r.gesture == Gesture::NONE, "a gesture is reported once");
  }
  // A touch held too long is not a tap: the hold countdown owns that case.
  {
    GestureTracker g{};
    gestureUpdate(g, true, 100, 120, 0, SWIPE, MOVE, TAP_MS);
    GestureResult r = gestureUpdate(g, false, 0, 0, 600, SWIPE, MOVE, TAP_MS);
    expect(r.gesture == Gesture::NONE, "a 600 ms press is not a tap");
  }
  // A short drag is neither a tap nor a swipe.
  {
    GestureTracker g{};
    gestureUpdate(g, true, 100, 120, 0, SWIPE, MOVE, TAP_MS);
    gestureUpdate(g, true, 100, 170, 100, SWIPE, MOVE, TAP_MS);
    GestureResult r = gestureUpdate(g, false, 0, 0, 200, SWIPE, MOVE, TAP_MS);
    expect(r.gesture == Gesture::NONE, "a 50 px drag is nothing");
  }
  // Long, mostly vertical travel is a swipe in the direction of travel, however slow.
  {
    GestureTracker g{};
    gestureUpdate(g, true, 160, 200, 0, SWIPE, MOVE, TAP_MS);
    gestureUpdate(g, true, 170, 60, 900, SWIPE, MOVE, TAP_MS);
    expect(gestureUpdate(g, false, 0, 0, 1000, SWIPE, MOVE, TAP_MS).gesture == Gesture::SWIPE_UP,
           "140 px upward is a swipe up");
    gestureUpdate(g, true, 160, 40, 2000, SWIPE, MOVE, TAP_MS);
    gestureUpdate(g, true, 150, 180, 2500, SWIPE, MOVE, TAP_MS);
    expect(gestureUpdate(g, false, 0, 0, 2600, SWIPE, MOVE, TAP_MS).gesture == Gesture::SWIPE_DOWN,
           "140 px downward is a swipe down");
  }
  // Travel that is more horizontal than vertical is not a swipe, even when it is long.
  {
    GestureTracker g{};
    gestureUpdate(g, true, 20, 40, 0, SWIPE, MOVE, TAP_MS);
    gestureUpdate(g, true, 300, 180, 300, SWIPE, MOVE, TAP_MS);
    expect(gestureUpdate(g, false, 0, 0, 400, SWIPE, MOVE, TAP_MS).gesture == Gesture::NONE,
           "a diagonal that is wider than tall is not a swipe");
  }
  // The swipe threshold is inclusive, and the last sample decides the distance.
  {
    GestureTracker g{};
    gestureUpdate(g, true, 100, 200, 0, SWIPE, MOVE, TAP_MS);
    gestureUpdate(g, true, 100, 20, 100, SWIPE, MOVE, TAP_MS);   // 180 px up ...
    gestureUpdate(g, true, 100, 70, 200, SWIPE, MOVE, TAP_MS);   // ... then back to 130
    expect(gestureUpdate(g, false, 0, 0, 300, SWIPE, MOVE, TAP_MS).gesture == Gesture::SWIPE_UP,
           "exactly the minimum travel counts, measured at lift-off");
  }
}

// /ota.txt holds the over-the-air update password: one line, never compiled in. A password that
// does not fit the buffer is refused rather than silently truncated, because a truncated secret
// would let a shorter guess through.
static void testOtaPassword() {
  char out[OTA_PASSWORD_MAX];
  expect(parseOtaPassword("correct horse battery\n", out, sizeof(out)) &&
             std::string(out) == "correct horse battery",
         "a plain line is the password");
  expect(parseOtaPassword("  padded-secret \r\n", out, sizeof(out)) &&
             std::string(out) == "padded-secret",
         "surrounding whitespace and the carriage return are stripped");
  expect(parseOtaPassword("\n\nsecond-line-secret\n", out, sizeof(out)) &&
             std::string(out) == "second-line-secret",
         "leading blank lines are skipped");
  expect(parseOtaPassword("first-line-secret\nignored\n", out, sizeof(out)) &&
             std::string(out) == "first-line-secret",
         "only the first non-empty line counts");
  expect(!parseOtaPassword("", out, sizeof(out)), "an empty file is no password");
  expect(!parseOtaPassword("\r\n \n", out, sizeof(out)), "a file of blank lines is no password");
  expect(!parseOtaPassword(nullptr, out, sizeof(out)), "a missing file is no password");
  expect(!parseOtaPassword("short\n", out, sizeof(out)), "fewer than eight characters is refused");
  expect(parseOtaPassword("12345678", out, sizeof(out)), "eight characters is the minimum");
  std::string tooLong(OTA_PASSWORD_MAX, 'x');
  expect(!parseOtaPassword(tooLong.c_str(), out, sizeof(out)),
         "a password that would not fit is refused, not truncated");
  std::string longest(OTA_PASSWORD_MAX - 1, 'y');
  expect(parseOtaPassword(longest.c_str(), out, sizeof(out)) && std::string(out) == longest,
         "the longest password that fits is accepted whole");
  expect(!parseOtaPassword("12345678", out, 4), "a buffer too small for the password is refused");
}

int main() {
  testSolarTimes();
  testUtcOffsetRecovery();
  testNightWindow();
  testNightWindowWrapsMidnight();
  testBrightnessRamp();
  testBandDecoder();
  testReadingDisagreement();
  testRateContext();
  testResolveHeartRate();
  testHighAlarmHeartRate();
  testLowAlarm();
  testCorrectedWindow();
  testHalvedOnsetStillAlarms();
  testVitalsCsv();
  testReadingMerge();
  testRadioHealth();
  testWifiFailover();
  testLiveRenderKey();
  testCriticalAlarm();
  testTraceWindow();
  testHoldGesture();
  testAlertLog();
  testContacts();
  testHistory();
  testDayBins();
  testGesture();
  testOtaPassword();
  if (failures) return EXIT_FAILURE;
  std::puts("firmware logic tests passed");
  return EXIT_SUCCESS;
}
