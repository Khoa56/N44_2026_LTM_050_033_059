#pragma once
#include <stdint.h>

// Doi thanh false de chay UDP co ban, khong doi ACK va khong gui lai.
constexpr bool ACK_ENABLED = true;
constexpr uint32_t ACK_TIMEOUT_MS = 600;
constexpr uint8_t MAX_SEND_ATTEMPTS = 3; // Tong cong 3 lan, gom lan gui dau.
static_assert(MAX_SEND_ATTEMPTS >= 1, "Need at least one send attempt");
static_assert(ACK_TIMEOUT_MS > 0, "ACK timeout must be positive");
