// provision_wifi.ino - writes one WiFi credential slot, the emergency contacts, or the over-the-air
// update password to the CYD's SD card over a serial prompt, so the card never has to leave the
// board.
//
// Credentials arrive over the serial connection at runtime. They are never compiled into this
// sketch and are never echoed back. The slot ("primary", "backup" or "backup2") is written through
// a verified temporary file, with the old target kept until the replacement has also been verified,
// so an interrupted write cannot leave the card without a usable file.
//
// Build: arduino-cli compile -b esp32:esp32:esp32 firmware/provision_wifi
// Flash: arduino-cli upload  -b esp32:esp32:esp32 -p /dev/cu.usbserial-XXXX firmware/provision_wifi
// Then:  arduino-cli monitor -p /dev/cu.usbserial-XXXX -c baudrate=115200 and follow the prompts.

#include <SD.h>
#include <SPI.h>
#include <WiFi.h>

#include "../cyd_vitals/config.h"
#include "../cyd_vitals/contacts.h"
#include "../cyd_vitals/ota_password.h"
#include "../cyd_vitals/wifi_failover.h"

constexpr char CONTACTS_PATH[] = "/contacts.txt";
constexpr uint32_t NETWORK_TEST_TIMEOUT_MS = 15'000;

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

// /contacts.txt holds the emergency numbers shown on the alarm screen. Like the credentials, they
// arrive over the serial connection and are never compiled in: one family's hospital numbers must
// never end up on a stranger's screen.
bool writeContacts(const String& first, const String& second) {
  File file = SD.open(CONTACTS_PATH, FILE_WRITE);
  if (!file) return false;
  file.println(first);
  if (second.length() > 0) file.println(second);
  file.flush();
  file.close();

  // Read back through the same parser the monitor uses, so "stored" means the monitor will show
  // these lines rather than merely that bytes reached the card.
  File check = SD.open(CONTACTS_PATH, FILE_READ);
  if (!check) return false;
  char buf[128];
  size_t n = check.readBytes(buf, sizeof(buf) - 1);
  buf[n] = '\0';
  check.close();

  Contacts parsed{};
  parseContacts(buf, parsed);
  int expected = second.length() > 0 ? 2 : 1;
  if (parsed.count != expected) return false;
  if (first.substring(0, CONTACT_LEN - 1) != String(parsed.line[0])) return false;
  if (expected == 2 && second.substring(0, CONTACT_LEN - 1) != String(parsed.line[1])) return false;
  for (int i = 0; i < parsed.count; ++i) Serial.printf("[sd] line %d: %s\n", i + 1, parsed.line[i]);
  return true;
}

// /ota.txt holds the password the monitor demands before accepting a firmware upload in its
// maintenance mode. Read back through the monitor's own parser, so "stored" means the monitor will
// accept it.
bool writeOtaPassword(const String& password) {
  File file = SD.open(OTA_PASSWORD_PATH, FILE_WRITE);
  if (!file) return false;
  file.println(password);
  file.flush();
  file.close();

  File check = SD.open(OTA_PASSWORD_PATH, FILE_READ);
  if (!check) return false;
  char buf[128];
  const size_t n = check.readBytes(buf, sizeof(buf) - 1);
  buf[n] = '\0';
  check.close();

  char parsed[OTA_PASSWORD_MAX];
  const bool ok = parseOtaPassword(buf, parsed, sizeof(parsed)) && password == String(parsed);
  memset(buf, 0, sizeof(buf));
  memset(parsed, 0, sizeof(parsed));
  return ok;
}

bool testSelectedNetwork(const String& ssid, const String& password) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  uint32_t startedMs = millis();
  while (WiFi.status() != WL_CONNECTED &&
         static_cast<uint32_t>(millis() - startedMs) < NETWORK_TEST_TIMEOUT_MS) {
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
  Serial.println("\n=== CYD SD card provisioner ===");

  String slotLabel = readLine("slot (primary, backup, backup2, contacts, or ota), then Enter:");

  sdSPI.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
  if (!SD.begin(SD_PIN_CS, sdSPI) || SD.cardType() == CARD_NONE) {
    Serial.println("[sd] init FAILED");
    return;
  }

  if (slotLabel == "contacts") {
    Serial.printf("up to two lines, %d characters each, shown verbatim on the alarm screen\n",
                  CONTACT_LEN - 1);
    String first = readLine("line 1, then Enter:");
    String second = readLine("line 2 (blank for none), then Enter:");
    if (first.length() == 0) {
      Serial.println("invalid empty line 1");
      return;
    }
    bool stored = writeContacts(first, second);
    Serial.printf("[sd] contacts stored=%s\n", stored ? "yes" : "no");
    return;
  }

  if (slotLabel == "ota") {
    Serial.printf("over-the-air update password: one line, at least %u characters\n",
                  static_cast<unsigned>(OTA_PASSWORD_MIN));
    String password = readLine("password, then Enter:");
    if (password.length() < OTA_PASSWORD_MIN) {
      Serial.println("too short");
      password = "";
      return;
    }
    bool stored = writeOtaPassword(password);
    password = "";
    Serial.printf("[sd] ota password stored=%s\n", stored ? "yes" : "no");
    return;
  }

  WifiSlot selectedSlot = WifiSlot::PRIMARY;
  if (!parseWifiSlot(slotLabel.c_str(), selectedSlot)) {
    Serial.println("invalid slot");
    return;
  }

  const char* target = wifiCredentialPath(selectedSlot);
  const char* temp = wifiCredentialTempPath(selectedSlot);
  const char* rollback = wifiCredentialRollbackPath(selectedSlot);

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
