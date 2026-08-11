#pragma once

#include <stddef.h>
#include <string.h>

// Emergency numbers shown on the alarm screen, read from /contacts.txt on the SD card.
//
// Lines are rendered verbatim so the owner controls the formatting - numbers sharing a prefix
// are best written as one compact line. The repository ships no default and contains no real
// numbers: this project is published for other people to build, and compiling one family's
// hospital numbers into it would put those numbers on a stranger's screen during an emergency.

constexpr int CONTACT_LINES = 2;
constexpr int CONTACT_LEN = 31;  // 30 characters plus terminator

struct Contacts {
  int count = 0;
  char line[CONTACT_LINES][CONTACT_LEN] = {};
};

inline void parseContacts(const char* text, Contacts& out) {
  out.count = 0;
  for (int i = 0; i < CONTACT_LINES; ++i) out.line[i][0] = '\0';
  if (text == nullptr) return;

  const char* cursor = text;
  while (*cursor != '\0' && out.count < CONTACT_LINES) {
    const char* end = cursor;
    while (*end != '\0' && *end != '\n') ++end;

    size_t len = static_cast<size_t>(end - cursor);
    while (len > 0 && (cursor[len - 1] == '\r' || cursor[len - 1] == ' ')) --len;

    if (len > 0) {
      if (len > CONTACT_LEN - 1) len = CONTACT_LEN - 1;
      memcpy(out.line[out.count], cursor, len);
      out.line[out.count][len] = '\0';
      ++out.count;
    }

    if (*end == '\0') break;
    cursor = end + 1;
  }
}
