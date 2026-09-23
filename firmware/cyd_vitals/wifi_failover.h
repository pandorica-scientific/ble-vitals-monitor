#pragma once

#include <stdint.h>
#include <string.h>

// The three WiFi credential slots on the SD card and the order they are tried in. Pure logic; the
// sketch supplies the function that actually connects. Slot labels are safe to log, paths are not
// secret, contents are.

enum class WifiSlot : uint8_t { PRIMARY, BACKUP, BACKUP2 };

inline const char* wifiCredentialPath(WifiSlot slot) {
  switch (slot) {
    case WifiSlot::PRIMARY: return "/wifi.txt";
    case WifiSlot::BACKUP: return "/wifi_backup.txt";
    case WifiSlot::BACKUP2: return "/wifi_backup2.txt";
  }
  return "";
}

// Written first by the provisioner, then renamed over the target once verified.
inline const char* wifiCredentialTempPath(WifiSlot slot) {
  switch (slot) {
    case WifiSlot::PRIMARY: return "/wifi.tmp";
    case WifiSlot::BACKUP: return "/wifi_backup.tmp";
    case WifiSlot::BACKUP2: return "/wifi_backup2.tmp";
  }
  return "";
}

// Holds the previous target until the replacement has been verified in place.
inline const char* wifiCredentialRollbackPath(WifiSlot slot) {
  switch (slot) {
    case WifiSlot::PRIMARY: return "/wifi.bak";
    case WifiSlot::BACKUP: return "/wifi_backup.bak";
    case WifiSlot::BACKUP2: return "/wifi_backup2.bak";
  }
  return "";
}

inline const char* wifiSlotLabel(WifiSlot slot) {
  switch (slot) {
    case WifiSlot::PRIMARY: return "primary";
    case WifiSlot::BACKUP: return "backup";
    case WifiSlot::BACKUP2: return "backup2";
  }
  return "unknown";
}

inline bool parseWifiSlot(const char* label, WifiSlot& out) {
  if (label == nullptr) return false;
  if (strcmp(label, "primary") == 0) {
    out = WifiSlot::PRIMARY;
  } else if (strcmp(label, "backup") == 0) {
    out = WifiSlot::BACKUP;
  } else if (strcmp(label, "backup2") == 0) {
    out = WifiSlot::BACKUP2;
  } else {
    return false;
  }
  return true;
}

// Tries the slots in order and stops at the first success.
template <typename TrySlot>
bool runWifiFailover(TrySlot trySlot) {
  if (trySlot(WifiSlot::PRIMARY)) return true;
  if (trySlot(WifiSlot::BACKUP)) return true;
  return trySlot(WifiSlot::BACKUP2);
}
