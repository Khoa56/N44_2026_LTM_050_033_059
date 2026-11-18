#include "reliable_udp.h"

void ReliableUdp::begin(const char *sessionId, const char *host, uint16_t destinationPort, uint16_t localPort) {
  snprintf(session, sizeof(session), "%s", sessionId);
  receiverHost = host;
  receiverPort = destinationPort;
  senderPort = localPort;
  metric("BOOT");
}

// Giu OLED va cam bien hoat dong khi mat Wi-Fi; khong luu hang doi so do cu.
void ReliableUdp::networkChanged(bool connected) {
  udp.stop();
  bound = false;
  if (connected) {
    validDestination = ack::validSession(session) &&
        (receiver.fromString(receiverHost)
#ifdef WOKWI_SIMULATION
         || WiFi.hostByName(receiverHost, receiver) == 1
#endif
        );
    if (!validDestination) status = "BAD IP";
  } else {
    ++stats.wifiDisconnects;
    if (pending.active) finishFailed("WIFI_LOST");
    status = "WIFI OFF";
  }
  metric(connected ? "WIFI_UP" : "WIFI_DOWN");
}

void ReliableUdp::publish(float temperature, float humidity, bool valid, uint32_t sampleMs) {
  ++stats.samples;
  if (WiFi.status() != WL_CONNECTED) {
    ++stats.skippedWifi;
    status = "WIFI OFF";
    metric("SKIP_WIFI");
    return;
  }
  if (!validDestination) {
    ++stats.skippedConfig;
    status = "BAD IP";
    metric("SKIP_CONFIG");
    return;
  }
  if (pending.active) {
    ++stats.skippedBusy;
    metric("SKIP_BUSY");
    return;
  }
  ++sequence;
  ++stats.logical;
  pending.begin(sequence);
  char temperatureText[16] = "null", humidityText[16] = "null";
  if (valid) {
    snprintf(temperatureText, sizeof(temperatureText), "%.2f", temperature);
    snprintf(humidityText, sizeof(humidityText), "%.2f", humidity);
  }
  // Tao DUNG MOT LAN: gui lai giu nguyen session, seq, thoi diem do va so do.
  const int length = snprintf(packet, sizeof(packet),
      "{\"type\":\"data\",\"session\":\"%s\",\"seq\":%lu,\"uptime_ms\":%lu,"
      "\"temperature_c\":%s,\"humidity_pct\":%s,\"sensor_valid\":%s,\"ack_required\":%s}",
      session, (unsigned long)sequence, (unsigned long)sampleMs,
      temperatureText, humidityText, valid ? "true" : "false", ACK_ENABLED ? "true" : "false");
  if (length <= 0 || size_t(length) >= sizeof(packet)) {
    finishFailed("PACKET_TOO_LONG");
    return;
  }
  metric("CREATED");
  transmit();
}

void ReliableUdp::transmit() {
  pending.sent(millis());
  ++stats.attempts;
  if (pending.attempts > 1) ++stats.retries;
  if (!bound) bound = udp.begin(senderPort) == 1;
  bool sent = false;
  if (bound && udp.beginPacket(receiver, receiverPort) == 1) {
    if (udp.print(packet) == strlen(packet)) sent = udp.endPacket() == 1;
  }
  if (sent) ++stats.localOk;
  else { udp.stop(); bound = false; }
  Serial.printf("TX seq=%lu attempt=%u local=%s\n", (unsigned long)sequence,
                pending.attempts, sent ? "OK" : "FAIL");
  if (!ACK_ENABLED) {
    pending.active = false;
    if (sent) { ++stats.unconfirmed; status = "LOCAL OK"; metric("UNCONFIRMED"); }
    else { ++stats.failed; status = "SEND FAIL"; metric("LOCAL_FAIL"); }
    return;
  }
  status = pending.attempts > 1 ? "RETRY" : "WAIT ACK";
}

void ReliableUdp::receiveAcks() {
  // Gioi han so goi xu ly moi luot de loop van cap nhat OLED/Wi-Fi.
  for (int count = 0; count < 4; ++count) {
    const int length = udp.parsePacket();
    if (length <= 0) break;
    char input[64], ackSession[17];
    const IPAddress sourceIp = udp.remoteIP();
    const uint16_t sourcePort = udp.remotePort();
    const int read = udp.read(input, sizeof(input));
    udp.flush(); // Bo phan du cua goi qua dai de ACK hop le sau do van doc duoc.
    uint32_t ackSequence = 0;
    if (length >= int(sizeof(input)) || read != length || sourceIp != receiver ||
        sourcePort != receiverPort || !ack::parse(input, read, ackSession, ackSequence) ||
        strcmp(ackSession, session) != 0) continue;
    const uint32_t now = millis();
    if (!pending.accept(ackSequence)) continue;
    ++stats.acked;
    status = "ACKED";
    // ACK khong co so thu tu lan gui: chi RTT lan dau la khong mo ho.
    metric("ACKED", uint32_t(now - pending.firstSentMs), pending.attempts == 1,
           uint32_t(now - pending.lastSentMs));
  }
}

void ReliableUdp::finishFailed(const char *reason) {
  pending.active = false;
  ++stats.failed;
  status = "NO ACK";
  metric(reason, pending.attempts ? uint32_t(millis() - pending.firstSentMs) : 0);
}

void ReliableUdp::tick() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (bound) receiveAcks(); // Doc ACK truoc khi quyet dinh gui lai.
  const auto action = pending.next(millis(), ACK_TIMEOUT_MS, MAX_SEND_ATTEMPTS);
  if (action == ack::Action::Send) transmit();
  if (action == ack::Action::Fail) finishFailed("NO_ACK");
}

// Dong METRIC duoc collect_serial.py chuyen thanh CSV; khong in mat khau Wi-Fi.
void ReliableUdp::metric(const char *event, uint32_t confirmMs, bool hasRtt, uint32_t rttMs) {
  char rtt[16] = "null";
  if (hasRtt) snprintf(rtt, sizeof(rtt), "%lu", (unsigned long)rttMs);
  Serial.printf(
      "METRIC {\"event\":\"%s\",\"session\":\"%s\",\"seq\":%lu,\"uptime_ms\":%lu,"
      "\"ack_enabled\":%s,\"attempt\":%u,\"confirm_ms\":%lu,\"rtt_ms\":%s,"
      "\"samples_total\":%lu,\"logical_total\":%lu,\"attempts_total\":%lu,\"tx_ok_total\":%lu,"
      "\"retries_total\":%lu,\"acked_total\":%lu,\"failed_total\":%lu,\"unconfirmed_total\":%lu,"
      "\"skipped_wifi\":%lu,\"skipped_busy\":%lu,\"skipped_config\":%lu,\"wifi_disconnects\":%lu,\"pending\":%s}\n",
      event, session, (unsigned long)sequence, (unsigned long)millis(), ACK_ENABLED ? "true" : "false",
      pending.attempts, (unsigned long)confirmMs, rtt,
      (unsigned long)stats.samples, (unsigned long)stats.logical, (unsigned long)stats.attempts,
      (unsigned long)stats.localOk, (unsigned long)stats.retries, (unsigned long)stats.acked,
      (unsigned long)stats.failed, (unsigned long)stats.unconfirmed, (unsigned long)stats.skippedWifi,
      (unsigned long)stats.skippedBusy, (unsigned long)stats.skippedConfig,
      (unsigned long)stats.wifiDisconnects, pending.active ? "true" : "false");
}
