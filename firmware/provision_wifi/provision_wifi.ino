// provision_wifi.ino — interactively write one Wi-Fi credential slot to the CYD SD card.
//
// Credentials arrive over the serial connection at runtime. They are never compiled into this
// sketch and are never echoed by the firmware. Select "primary" for /wifi.txt, "backup" for
// /wifi_backup.txt, or "backup2" for /wifi_backup2.txt. The selected file is written through a
// verified temporary file, with the old target retained until the replacement has also been
// verified.

#include <SPI.h>
#include <SD.h>
#include <WiFi.h>

#include "../cyd_vitals/wifi_failover.h"

#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23
#define SD_CS 5

SPIClass sdSPI(VSPI);

String readLine(const char* prompt) {
  Serial.println(prompt);
  String value;
  while (true) {
    while (!Serial.available()) delay(10);
    char next = static_cast<char>(Serial.read());
    if (next == '\n') break;
    if (next != '\r') value += next;
  }
  value.trim();
  return value;
}

bool readCredentialFile(const char* path, String& ssid, String& password) {
  File file = SD.open(path, FILE_READ);
  if (!file) return false;
  ssid = file.readStringUntil('\n');
  password = file.readStringUntil('\n');
  file.close();
  ssid.trim();
  password.trim();
  return ssid.length() > 0;
}

bool verifyCredentialFile(const char* path, const String& expectedSsid,
                          const String& expectedPassword) {
  String storedSsid;
  String storedPassword;
  bool readable = readCredentialFile(path, storedSsid, storedPassword);
  bool matches = readable && storedSsid == expectedSsid &&
                 storedPassword == expectedPassword;
  storedPassword = "";
  return matches;
}

bool writeCredentialTransactional(const char* target, const char* temp,
                                  const char* backup, const String& ssid,
                                  const String& password) {
  // Recover an interrupted earlier replacement before beginning a new one.
  if (SD.exists(backup)) {
    if (!SD.exists(target)) {
      if (!SD.rename(backup, target)) {
        Serial.println("[sd] prior backup restore failed");
        return false;
      }
    } else {
      String currentSsid;
      String currentPassword;
      bool targetReadable = readCredentialFile(target, currentSsid, currentPassword);
      currentPassword = "";
      if (!targetReadable) {
        if (!SD.remove(target) || !SD.rename(backup, target)) {
          Serial.println("[sd] interrupted transaction recovery failed");
          return false;
        }
      } else if (!SD.remove(backup)) {
        Serial.println("[sd] stale backup removal failed");
        return false;
      }
    }
  }

  if (SD.exists(temp) && !SD.remove(temp)) {
    Serial.println("[sd] stale temp removal failed");
    return false;
  }

  File file = SD.open(temp, FILE_WRITE);
  if (!file) return false;
  size_t ssidBytes = file.println(ssid);
  size_t passwordBytes = file.println(password);
  file.flush();
  file.close();
  if (ssidBytes == 0 || passwordBytes == 0 ||
      !verifyCredentialFile(temp, ssid, password)) {
    SD.remove(temp);
    return false;
  }

  bool hadTarget = SD.exists(target);
  if (hadTarget && !SD.rename(target, backup)) {
    SD.remove(temp);
    return false;
  }
  if (!SD.rename(temp, target)) {
    if (hadTarget && !SD.rename(backup, target)) {
      Serial.println("[sd] rollback failed");
    }
    return false;
  }
  if (!verifyCredentialFile(target, ssid, password)) {
    SD.remove(target);
    if (hadTarget && !SD.rename(backup, target)) {
      Serial.println("[sd] rollback failed");
    }
    return false;
  }
  if (hadTarget && !SD.remove(backup)) {
    Serial.println("[sd] stored; old backup cleanup deferred");
  }
  return true;
}

bool testSelectedNetwork(const String& ssid, const String& password) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  uint32_t startedMs = millis();
  while (WiFi.status() != WL_CONNECTED &&
         static_cast<uint32_t>(millis() - startedMs) < 15000) {
    delay(250);
  }
  bool connected = WiFi.status() == WL_CONNECTED;
  Serial.println(connected ? "[wifi] selected slot connected"
                           : "[wifi] selected slot timeout");
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  return connected;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Wi-Fi credential provisioner ===");

  String slotLabel = readLine("slot (primary, backup, or backup2), then Enter:");
  WifiSlot selectedSlot = WifiSlot::PRIMARY;
  if (!parseWifiSlot(slotLabel.c_str(), selectedSlot)) {
    Serial.println("invalid slot");
    return;
  }

  const char* target = wifiCredentialPath(selectedSlot);
  const char* temp = wifiCredentialTempPath(selectedSlot);
  const char* rollback = wifiCredentialRollbackPath(selectedSlot);

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI) || SD.cardType() == CARD_NONE) {
    Serial.println("[SD] init FAILED");
    return;
  }

  String ssid = readLine("SSID, then Enter:");
  String password = readLine("password, then Enter:");
  if (ssid.length() == 0) {
    Serial.println("invalid empty SSID");
    password = "";
    return;
  }

  bool stored = writeCredentialTransactional(target, temp, rollback, ssid, password);
  Serial.printf("[sd] slot=%s stored=%s ssid_len=%u password_len=%u\n",
                wifiSlotLabel(selectedSlot), stored ? "yes" : "no",
                static_cast<unsigned>(ssid.length()),
                static_cast<unsigned>(password.length()));
  if (stored) testSelectedNetwork(ssid, password);
  password = "";
}

void loop() { delay(1000); }
