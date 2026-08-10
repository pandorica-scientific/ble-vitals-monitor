#include "../firmware/cyd_vitals/band_protocol.h"
#include "../firmware/cyd_vitals/live_render_key.h"
#include "../firmware/cyd_vitals/radio_health.h"
#include "../firmware/cyd_vitals/wifi_failover.h"

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

int main() {
  testBandDecoder();
  testReadingMerge();
  testRadioHealth();
  testWifiFailover();
  testLiveRenderKey();
  if (failures) return EXIT_FAILURE;
  std::puts("firmware logic tests passed");
  return EXIT_SUCCESS;
}
