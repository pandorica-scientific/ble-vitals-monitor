// provision_wifi.ino — one-time helper: write /wifi.txt to the SD card, then verify.
//
// The SD card is wired to the ESP32 (not to your computer), so this is how you put your WiFi
// credentials on the card without a card reader:
//   1. Put your SSID/password in the two fields below.
//   2. Flash this sketch once and open the serial monitor — it writes /wifi.txt and confirms.
//   3. Flash the main firmware (cyd_vitals), and CLEAR your credentials back out of this file.
//
// The main firmware uses these only to sync the clock over NTP at boot, then turns WiFi off.
#include <SPI.h>
#include <SD.h>
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23
#define SD_CS 5
SPIClass sdSPI(VSPI);

const char* SSID = "YOUR_WIFI_NAME";       // <-- your 2.4 GHz network name
const char* PASS = "YOUR_WIFI_PASSWORD";   // <-- your WiFi password

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("\n=== provision /wifi.txt ===");
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI)) { Serial.println("[SD] init FAILED"); return; }
  File f = SD.open("/wifi.txt", FILE_WRITE);      // FILE_WRITE truncates/overwrites
  if (!f) { Serial.println("[SD] open for write FAILED"); return; }
  f.println(SSID); f.println(PASS); f.close();
  Serial.println("[SD] wrote /wifi.txt");
  // verify
  File r = SD.open("/wifi.txt");
  String s = r.readStringUntil('\n'); s.trim();
  String p = r.readStringUntil('\n'); p.trim();
  r.close();
  Serial.printf("[verify] ssid='%s'  pass=(%d chars, first=%c last=%c)\n",
                s.c_str(), p.length(), p.length()?p[0]:'?', p.length()?p[p.length()-1]:'?');
  Serial.println(s == SSID && p == PASS ? "=== OK: /wifi.txt matches ===" : "=== MISMATCH ===");
}
void loop() { delay(1000); }
