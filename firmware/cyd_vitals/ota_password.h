#pragma once

#include <stddef.h>
#include <string.h>

// The over-the-air update password: one line in /ota.txt on the SD card, never compiled in. Pure
// logic; the sketch reads the file and the provisioner writes it.

constexpr char OTA_PASSWORD_PATH[] = "/ota.txt";
constexpr size_t OTA_PASSWORD_MIN = 8;
constexpr size_t OTA_PASSWORD_MAX = 64;   // buffer size, including the terminator

// Copies the first non-empty line of `text`, trimmed of spaces, tabs and line endings, into `out`.
// False when there is none, when it is shorter than OTA_PASSWORD_MIN, or when it would not fit: a
// truncated secret would be a different, weaker secret.
inline bool parseOtaPassword(const char* text, char* out, size_t cap) {
  if (out == nullptr || cap == 0) return false;
  out[0] = '\0';
  if (text == nullptr) return false;

  const char* cursor = text;
  while (*cursor != '\0') {
    const char* end = cursor;
    while (*end != '\0' && *end != '\n') ++end;

    const char* start = cursor;
    while (start < end && (*start == ' ' || *start == '\t' || *start == '\r')) ++start;
    const char* stop = end;
    while (stop > start && (stop[-1] == ' ' || stop[-1] == '\t' || stop[-1] == '\r')) --stop;

    const size_t len = static_cast<size_t>(stop - start);
    if (len > 0) {
      if (len < OTA_PASSWORD_MIN || len > cap - 1) return false;
      memcpy(out, start, len);
      out[len] = '\0';
      return true;
    }
    if (*end == '\0') break;
    cursor = end + 1;
  }
  return false;
}
