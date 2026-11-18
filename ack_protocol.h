#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Dung chung cho ESP32 va may tinh; ACK = ACK,<session 16 ky tu hex>,<seq>.
namespace ack {
inline bool validSession(const char *session) {
  if (strlen(session) != 16) return false;
  for (int i = 0; i < 16; ++i) {
    const char c = session[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return false;
  }
  return true;
}

inline bool parse(const char *data, size_t length, char *session, uint32_t &sequence) {
  if (length < 22 || length > 31 || memcmp(data, "ACK,", 4) != 0 || data[20] != ',')
    return false;
  memcpy(session, data + 4, 16);
  session[16] = '\0';
  if (!validSession(session)) return false;
  uint64_t number = 0;
  for (size_t i = 21; i < length; ++i) {
    if (data[i] < '0' || data[i] > '9') return false;
    number = number * 10 + unsigned(data[i] - '0');
    if (number > UINT32_MAX) return false;
  }
  sequence = uint32_t(number);
  return true;
}

inline int format(char *output, size_t capacity, const char *session, uint32_t sequence) {
  return snprintf(output, capacity, "ACK,%s,%lu", session, (unsigned long)sequence);
}

enum class Action { Wait, Send, Fail };

// Stop-and-wait: chi doi ACK cho mot ban tin tai mot thoi diem.
struct Pending {
  bool active = false;
  uint32_t sequence = 0, firstSentMs = 0, lastSentMs = 0;
  unsigned attempts = 0;

  void begin(uint32_t seq) { active = true; sequence = seq; attempts = 0; }
  void sent(uint32_t now) {
    if (attempts == 0) firstSentMs = now;
    lastSentMs = now;
    ++attempts;
  }
  Action next(uint32_t now, uint32_t timeoutMs, unsigned maxAttempts) const {
    if (!active) return Action::Wait;
    if (attempts == 0) return Action::Send;
    if (uint32_t(now - lastSentMs) < timeoutMs) return Action::Wait;
    return attempts < maxAttempts ? Action::Send : Action::Fail;
  }
  bool accept(uint32_t seq) {
    if (!active || attempts == 0 || seq != sequence) return false;
    active = false;
    return true;
  }
};
} // namespace ack
