// bandsniff.ino — classic ESP32 (BLE 4.2) test: can it receive the Baby Sensor
// Relax band's advertisements? RECEIVE-ONLY passive test. Never connects/writes.
//
// GO/NO-GO logic:
//   * If this prints a device named "BS01" or manufacturer data starting F5 03  -> the band
//     uses LEGACY advertising and the classic ESP32 CAN sniff it.
//   * If it prints LOTS of other BLE devices but NEVER the band (while the Mac sees it)
//     -> the band uses BLE 5 EXTENDED advertising; classic ESP32 can't. Need ESP32-C3/C6/S3
//        or nRF52840.
//
// Build: arduino-cli compile -b esp32:esp32:esp32 esp32_bandsniff
// Flash: arduino-cli upload  -b esp32:esp32:esp32 -p /dev/cu.usbserial-XXXX esp32_bandsniff
// View : arduino-cli monitor -p /dev/cu.usbserial-XXXX -c baudrate=115200

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

static uint32_t total = 0, withMfg = 0, bandHits = 0;

static String toHex(const String& s) {
  static const char* h = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < s.length(); i++) { uint8_t c = (uint8_t)s[i]; out += h[c >> 4]; out += h[c & 0xF]; }
  return out;
}

class CB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    total++;
    String name = dev.haveName() ? dev.getName() : String();
    String mfg  = dev.haveManufacturerData() ? dev.getManufacturerData() : String();
    bool isBand = name.startsWith("BS01") ||
                  (mfg.length() >= 2 && (uint8_t)mfg[0] == 0xF5 && (uint8_t)mfg[1] == 0x03);
    bool isBase = name.startsWith("BG01") ||
                  (mfg.length() >= 2 && (uint8_t)mfg[0] == 0xF5 && (uint8_t)mfg[1] == 0x04);
    if (mfg.length()) withMfg++;
    if (isBand) {
      bandHits++;
      Serial.printf(">>> BAND! rssi=%d name=%s mfg=%s\n",
                    dev.getRSSI(), name.c_str(), toHex(mfg).c_str());
    } else if (isBase) {
      Serial.printf("  [base]  rssi=%d name=%s mfg=%s\n",
                    dev.getRSSI(), name.c_str(), toHex(mfg).c_str());
    } else if (mfg.length()) {
      Serial.printf("  (other) rssi=%d name=%-16s mfg=%s\n",
                    dev.getRSSI(), name.length() ? name.c_str() : "-", toHex(mfg).c_str());
    }
  }
};

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32 band-sniff test (classic BLE 4.2) ===");
  BLEDevice::init("");
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new CB(), true /*wantDuplicates*/);
  scan->setActiveScan(true);          // request scan response (gets the name)
  scan->setInterval(100);
  scan->setWindow(99);                // ~99% duty -> best chance to catch the band
  scan->start(0, nullptr, false);     // continuous
  Serial.println("scanning continuously...");
}

void loop() {
  delay(5000);
  Serial.printf("[stat] adverts=%u withMfg=%u  BAND-hits=%u  %s\n",
                total, withMfg, bandHits,
                bandHits ? "=> LEGACY adv, ESP32 WORKS" :
                           "=> no band yet (out of range OR extended adv)");
}
