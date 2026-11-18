#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_system.h>
#include <esp32-hal-rgb-led.h>
#include <math.h>
#include "config.h"
#include "reliable_udp.h"

#ifdef WOKWI_SIMULATION
DHT dht(DHT_DATA_PIN, DHT22);
constexpr float minTemperature = -40;
constexpr float maxTemperature = 80;
#else
DHT dht(DHT_DATA_PIN, DHT11);
constexpr float minTemperature = 0;
constexpr float maxTemperature = 50;
#endif
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
ReliableUdp transport;
float temperatureC = NAN, humidityPct = NAN;
bool sensorValid = false, oledReady = false;
const char *sensorStatus = "WAIT";
unsigned long lastSample = 0, lastDisplay = 0, lastRetry = 0;

void initOled() {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  const uint8_t addresses[] = {0x3C, 0x3D};
  for (uint8_t address : addresses) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() != 0) continue;
    oledReady = oled.begin(SSD1306_SWITCHCAPVCC, address, true, false);
    if (!oledReady) continue;
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextWrap(false);
    return;
  }
  Serial.println("OLED not found; sensor and network still run.");
}

// Doc theo chu ky, du Wi-Fi dang mat hay dang doi ACK.
void readSensor() {
  const uint32_t sampleMs = millis();
  temperatureC = dht.readTemperature();
  humidityPct = dht.readHumidity();
  sensorValid = isfinite(temperatureC) && isfinite(humidityPct) &&
                temperatureC >= minTemperature && temperatureC <= maxTemperature &&
                humidityPct >= 0 && humidityPct <= 100;
  sensorStatus = sensorValid ? "OK" : "ERROR";
  Serial.printf("Sensor=%s T=%.2f C H=%.2f %%\n", sensorStatus, temperatureC, humidityPct);
  transport.publish(temperatureC, humidityPct, sensorValid, sampleMs);
}

void maintainWifi() {
  if (WIFI_SSID[0] == '\0') return;
  static bool wasConnected = false;
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != wasConnected) {
    transport.networkChanged(connected);
    Serial.println(connected ? "WiFi connected" : "WiFi disconnected");
    if (connected) Serial.println(WiFi.localIP());
    wasConnected = connected;
  }
  if (!connected && millis() - lastRetry >= WIFI_RETRY_MS) {
    lastRetry = millis();
    WiFi.reconnect();
  }
}

void updateOled() {
  if (!oledReady) return;
  oled.clearDisplay();
  oled.setCursor(0, 0);
  if (sensorValid)
    oled.printf("Temp: %.1f C\nHumidity: %.1f %%\n", temperatureC, humidityPct);
  else oled.println("Temp: -- C\nHumidity: -- %");
  oled.printf("Sensor: %s\n", sensorStatus);
  oled.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "ON" : "OFF");
  oled.printf("Seq: %lu\n", (unsigned long)transport.sequence);
  oled.printf("TX:%lu ACK:%lu\n", (unsigned long)transport.stats.localOk,
              (unsigned long)transport.stats.acked);
  oled.printf("Last: %s\n", transport.status);
  oled.display();
}

void setup() {
  Serial.begin(115200);
  neopixelWrite(8, 0, 0, 0);
  delay(150);
  dht.begin();
  initOled();
  if (WIFI_SSID[0] != '\0') {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  } else {
    Serial.println("Missing Wi-Fi config: copy network.example.h to network.local.h.");
  }
  char session[17];
  snprintf(session, sizeof(session), "%08lx%08lx",
           (unsigned long)esp_random(), (unsigned long)esp_random());
  transport.begin(session, RECEIVER_HOST, RECEIVER_PORT, ESP32_LOCAL_PORT);
  lastSample = lastRetry = millis();
  updateOled();
}

void loop() {
  maintainWifi();
  transport.tick(); // Nhan ACK va gui lai khi het han, khong dung while de cho.
  if (millis() - lastSample >= SAMPLE_INTERVAL_MS) {
    readSensor();
    lastSample = millis();
  }
  if (millis() - lastDisplay >= 500) {
    lastDisplay = millis();
    updateOled();
  }
  delay(5);
}
