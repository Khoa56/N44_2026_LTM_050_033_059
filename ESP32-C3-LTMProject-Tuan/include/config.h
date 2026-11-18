#pragma once
#include <stdint.h>

#if defined(WOKWI_SIMULATION)
constexpr char WIFI_SSID[] = "Wokwi-GUEST";
constexpr char WIFI_PASSWORD[] = "";
constexpr char RECEIVER_HOST[] = "host.wokwi.internal";
#elif __has_include("network.local.h")
#include "network.local.h"
#else
// Sao chép network.example.h thành network.local.h để kết nối từ bo thật.
constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";
constexpr char RECEIVER_HOST[] = "";
#endif

constexpr uint16_t RECEIVER_PORT = 8080;
constexpr uint16_t ESP32_LOCAL_PORT = 4210;
constexpr uint8_t OLED_SDA_PIN = 4;
constexpr uint8_t OLED_SCL_PIN = 5;
constexpr uint8_t DHT_DATA_PIN = 6;
constexpr uint32_t SAMPLE_INTERVAL_MS = 2500;
constexpr uint32_t WIFI_RETRY_MS = 10000;
