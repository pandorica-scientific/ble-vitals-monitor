// bandsniff.ino - reception test: can this classic ESP32 (BLE 4.2) hear the wristband at all?
//
// Receive-only. It lists every advertising device it sees and counts the wristband's frames.
//
//   Prints ">>> BAND" lines            -> the band uses legacy advertising; this ESP32 can decode it
//   Prints many other devices, never   -> the band uses BLE 5 extended advertising and this chip
//   the band (while a phone sees it)      cannot receive it; a BLE 5 receiver is needed
//
// Build: arduino-cli compile -b esp32:esp32:esp32 firmware/bandsniff
// Flash: arduino-cli upload  -b esp32:esp32:esp32 -p /dev/cu.usbserial-XXXX firmware/bandsniff
// View:  arduino-cli monitor -p /dev/cu.usbserial-XXXX -c baudrate=115200

#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>

#include "../cyd_vitals/band_protocol.h"

constexpr uint8_t BAND_DEVICE_BASE = 0x04;

static uint32_t g_total = 0;
static uint32_t g_withManufacturerData = 0;
static uint32_t g_bandHits = 0;

static String toHex(const String& s) {
  static const char* digits = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < s.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(s[i]);
    out += digits[c >> 4];
    out += digits[c & 0xF];
  }
  return out;
}

static bool hasPrefix(const String& mfg, uint8_t deviceType) {
  return mfg.length() >= 2 && static_cast<uint8_t>(mfg[0]) == BAND_FRAME_MARKER &&
         static_cast<uint8_t>(mfg[1]) == deviceType;
}

class SniffCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice device) override {
    ++g_total;
    const String name = device.haveName() ? device.getName() : String();
    const String mfg = device.haveManufacturerData() ? device.getManufacturerData() : String();
    if (mfg.length()) ++g_withManufacturerData;

    const bool isBand = name.startsWith("BS01") || hasPrefix(mfg, BAND_DEVICE_WRISTBAND);
    const bool isBase = name.startsWith("BG01") || hasPrefix(mfg, BAND_DEVICE_BASE);
    if (isBand) {
      ++g_bandHits;
      Serial.printf(">>> BAND  rssi=%d name=%s mfg=%s\n", device.getRSSI(), name.c_str(),
                    toHex(mfg).c_str());
    } else if (isBase) {
      Serial.printf("  [base]  rssi=%d name=%s mfg=%s\n", device.getRSSI(), name.c_str(),
                    toHex(mfg).c_str());
    } else if (mfg.length()) {
      Serial.printf("  (other) rssi=%d name=%-16s mfg=%s\n", device.getRSSI(),
                    name.length() ? name.c_str() : "-", toHex(mfg).c_str());
    }
  }
};

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32 band-sniff test (classic BLE 4.2) ===");
  BLEDevice::init("");
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new SniffCallbacks(), true /* report duplicates */);
  scan->setActiveScan(true);   // requests the scan response, which carries the device name
  scan->setInterval(100);
  scan->setWindow(99);
  scan->start(0, nullptr, false);
  Serial.println("scanning continuously...");
}

void loop() {
  delay(5000);
  Serial.printf("[stat] adverts=%lu withMfg=%lu band=%lu  %s\n",
                static_cast<unsigned long>(g_total),
                static_cast<unsigned long>(g_withManufacturerData),
                static_cast<unsigned long>(g_bandHits),
                g_bandHits ? "=> legacy advertising, this ESP32 works"
                           : "=> no band yet (out of range, or extended advertising)");
}
