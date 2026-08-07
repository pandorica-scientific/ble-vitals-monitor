// reader_serial.ino — standalone Baby Sensor Relax reader on the ESP32 (serial only, no display).
// RECEIVE-ONLY: passive BLE scan; never connects/writes/pairs. Cannot affect the base link.
// Decodes the wristband's 23-byte advertisement and prints vitals over serial.
//
// Frame (device type 0x03 = wristband):
//   b0 F5 marker | b1 03 type | b2 counter | b4 signal | b7 body-raw | b9 skin-raw
//   b10 HR bpm | b13 SpO2 % | b16-21 MAC(LE) | b22 checksum
//   skin_C = b9/2 + 0.5   body_C = b7/2 + 0.5   (blank if implausible)
//
// NOTE: thresholds below are ARBITRARY and NOT medical advice. This is a hobby reader,
// not a safety device. Keep using the official monitor.

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// --- configurable alert thresholds (informational only) ---
#define HR_LOW   90
#define HR_HIGH  180
#define SPO2_LOW 90
#define SKIN_MIN 28.0   // plausibility gate
#define SKIN_MAX 42.0

static int lastSeq = -1;

class CB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (!dev.haveManufacturerData()) return;
    String m = dev.getManufacturerData();
    if (m.length() < 23) return;
    const uint8_t* b = (const uint8_t*)m.c_str();
    if (b[0] != 0xF5 || b[1] != 0x03) return;      // wristband frames only

    int seq = b[2];
    if (seq == lastSeq) return;                     // one line per new reading
    lastSeq = seq;

    int hr = b[10], spo2 = b[13], sig = b[4];
    float skin = b[9] / 2.0f + 0.5f;
    float body = b[7] / 2.0f + 0.5f;
    bool tempOK = (skin >= SKIN_MIN && skin <= SKIN_MAX);

    char skinS[8], bodyS[8];
    if (tempOK) { snprintf(skinS, 8, "%.1fC", skin); snprintf(bodyS, 8, "%.1fC", body); }
    else        { strcpy(skinS, "  --"); strcpy(bodyS, "  --"); }

    // alert flags (informational)
    String flag = "";
    if (hr < HR_LOW)  flag += " HR-LOW";
    if (hr > HR_HIGH) flag += " HR-HIGH";
    if (spo2 < SPO2_LOW) flag += " SpO2-LOW";

    Serial.printf("HR %3d bpm | SpO2 %3d%% | skin %-6s | body %-6s | sig %2d | rssi %d%s\n",
                  hr, spo2, skinS, bodyS, sig, dev.getRSSI(),
                  flag.length() ? ("  <<<ALERT:" + flag).c_str() : "");
  }
};

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== Baby Sensor Relax — ESP32 reader (RECEIVE-ONLY) ===");
  Serial.println("thresholds are informational only, NOT medical. one line per new reading.\n");
  BLEDevice::init("");
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new CB(), true);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->start(0, nullptr, false);
}

void loop() { delay(1000); }
