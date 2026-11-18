#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "ack_config.h"
#include "ack_protocol.h"

struct TransportStats {
  uint32_t samples = 0, logical = 0, attempts = 0, localOk = 0, retries = 0;
  uint32_t acked = 0, failed = 0, unconfirmed = 0;
  uint32_t skippedWifi = 0, skippedBusy = 0, skippedConfig = 0, wifiDisconnects = 0;
};

class ReliableUdp {
public:
  TransportStats stats;
  uint32_t sequence = 0;
  const char *status = "WAIT";

  void begin(const char *sessionId, const char *host, uint16_t destinationPort, uint16_t localPort);
  void networkChanged(bool connected);
  void publish(float temperature, float humidity, bool valid, uint32_t sampleMs);
  void tick();

private:
  WiFiUDP udp;
  IPAddress receiver;
  const char *receiverHost = "";
  uint16_t receiverPort = 0, senderPort = 0;
  bool validDestination = false, bound = false;
  char session[17] = {}, packet[288] = {};
  ack::Pending pending;

  void transmit();
  void receiveAcks();
  void finishFailed(const char *reason);
  void metric(const char *event, uint32_t confirmMs = 0, bool hasRtt = false, uint32_t rttMs = 0);
};
