// reader_serial.ino - Baby Sensor Relax reader for a bare ESP32: decoded vitals over serial, no
// display, no SD card.
//
// Receive-only: a passive BLE scan that never connects, pairs or transmits, so it cannot affect the
// wristband's link to its base. Decoding is shared with the main firmware through band_protocol.h,
// so this sketch cannot drift from it.
//
// Thresholds below are arbitrary and not medical advice. Hobby reader, not a safety device.
//
// Build: arduino-cli compile -b esp32:esp32:esp32 firmware/reader_serial
// Flash: arduino-cli upload  -b esp32:esp32:esp32 -p /dev/cu.usbserial-XXXX firmware/reader_serial
// View:  arduino-cli monitor -p /dev/cu.usbserial-XXXX -c baudrate=115200

#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>

#include "../cyd_vitals/band_protocol.h"

// Informational only: they colour nothing and alarm nothing, they just tag the line.
constexpr int HR_LOW = 90;
constexpr int HR_HIGH = 180;
constexpr int SPO2_LOW = 90;

static int g_lastSequence = -1;

class BandScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice device) override {
    if (!device.haveManufacturerData()) return;
    const String manufacturer = device.getManufacturerData();
    BandReading reading{};
    if (!decodeBandFrame(reinterpret_cast<const uint8_t*>(manufacturer.c_str()),
                         manufacturer.length(), reading)) {
      return;
    }
    if (reading.sequence == g_lastSequence) return;   // one line per new measurement
    g_lastSequence = reading.sequence;

    char skin[8];
    if (reading.skinValid) {
      snprintf(skin, sizeof(skin), "%.1fC", static_cast<double>(reading.skinC));
    } else {
      snprintf(skin, sizeof(skin), "--");
    }

    String flags;
    if (reading.heartRate < HR_LOW) flags += " HR-LOW";
    if (reading.heartRate > HR_HIGH) flags += " HR-HIGH";
    if (reading.oxygenSaturation < SPO2_LOW) flags += " SpO2-LOW";
    if (bandReadingDisagrees(reading.heartRate, reading.beatMs)) flags += " HR-DISAGREE";

    Serial.printf("HR %3d bpm | beat %4d ms | SpO2 %3d%% | skin %-6s | sig %2d | rssi %d%s%s\n",
                  reading.heartRate, reading.beatMs, reading.oxygenSaturation, skin,
                  reading.signal, device.getRSSI(), flags.length() ? "  <<<" : "", flags.c_str());
  }
};

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== Baby Sensor Relax - ESP32 serial reader (receive-only) ===");
  Serial.println("flags are informational only, not medical. one line per new measurement.\n");
  BLEDevice::init("");
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new BandScanCallbacks(), true);
  scan->setActiveScan(false);   // passive: never transmit a scan request
  scan->setInterval(100);
  scan->setWindow(100);
  scan->start(0, nullptr, false);
}

void loop() { delay(1000); }
