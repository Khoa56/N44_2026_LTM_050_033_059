// Windows C++17. Build:
// g++ -std=c++17 -O2 -static udp_ack_receiver.cpp -o udp_ack_receiver.exe -lws2_32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <chrono>
#include <csignal>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "third_party/json.hpp"
#include "../include/ack_protocol.h"

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t running = 1;
void stop(int) { running = 0; }
long long epochMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

unsigned number(const std::string &text, unsigned maximum) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
    throw std::runtime_error("Expected a non-negative integer: " + text);
  const auto value = std::stoull(text);
  if (value > maximum) throw std::runtime_error("Number is too large: " + text);
  return unsigned(value);
}

struct Options {
  unsigned port = 5005, duration = 0, dropAckEvery = 0, dropDataEvery = 0, ackDelayMs = 0;
  bool dropAllAcks = false;
  std::string output, forwardIp;
  unsigned forwardPort = 5005;
};

Options options(int argc, char **argv) {
  Options result;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--drop-ack-all") { result.dropAllAcks = true; continue; }
    if (flag == "--help") {
      std::cout << "udp_ack_receiver.exe [5005] --out results/run01 [--duration 150]\n"
                   "  --drop-ack-every N   Bo ACK lan dau cua moi seq chia het cho N\n"
                   "  --drop-data-every N  Bo lan nhan dau cua moi seq chia het cho N\n"
                   "  --drop-ack-all       Khong tra ACK (thu het so lan gui lai)\n"
                   "  --ack-delay-ms N     Tre ACK, 0..10000 ms\n"
                   "  --forward-ip IP --forward-port PORT   Chuyen tiep hai chieu qua VPN\n";
      std::exit(0);
    }
    if (i == 1 && flag.rfind("--", 0) != 0) { result.port = number(flag, 65535); continue; }
    if (i + 1 >= argc) throw std::runtime_error("Missing value for " + flag);
    const std::string value = argv[++i];
    if (flag == "--port") result.port = number(value, 65535);
    else if (flag == "--out") result.output = value;
    else if (flag == "--duration") result.duration = number(value, 86400);
    else if (flag == "--drop-ack-every") result.dropAckEvery = number(value, 1000000);
    else if (flag == "--drop-data-every") result.dropDataEvery = number(value, 1000000);
    else if (flag == "--ack-delay-ms") result.ackDelayMs = number(value, 10000);
    else if (flag == "--forward-ip") result.forwardIp = value;
    else if (flag == "--forward-port") result.forwardPort = number(value, 65535);
    else throw std::runtime_error("Unknown option: " + flag);
  }
  if (!result.port || !result.forwardPort || result.output.empty())
    throw std::runtime_error("Need valid port and --out NEW_RESULTS_FOLDER.");
  if (!result.forwardIp.empty() && (result.dropAckEvery || result.dropDataEvery || result.dropAllAcks || result.ackDelayMs))
    throw std::runtime_error("Use loss/delay simulation on the FINAL receiver, not the relay.");
  return result;
}

struct Wsa {
  Wsa() { WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("WSAStartup failed"); }
  ~Wsa() { WSACleanup(); }
};
struct Socket {
  SOCKET value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};

bool validData(const Json &data) {
  if (!data.is_object()) return false;
  for (auto field : {"session", "seq", "uptime_ms", "temperature_c", "humidity_pct", "sensor_valid"})
    if (!data.contains(field)) return false;
  if (!data["session"].is_string() || !data["seq"].is_number_unsigned() ||
      !data["uptime_ms"].is_number_unsigned() || !data["sensor_valid"].is_boolean()) return false;
  const auto session = data["session"].get<std::string>();
  if (session.size() != 16 || !ack::validSession(session.c_str()) || data["seq"].get<uint64_t>() > UINT32_MAX ||
      data["uptime_ms"].get<uint64_t>() > UINT32_MAX) return false;
  if (data.contains("type") && data["type"] != "data") return false;
  if (data.contains("ack_required") && !data["ack_required"].is_boolean()) return false;
  if (!data["sensor_valid"].get<bool>())
    return data["temperature_c"].is_null() && data["humidity_pct"].is_null();
  if (!data["temperature_c"].is_number() || !data["humidity_pct"].is_number()) return false;
  const double temperature = data["temperature_c"].get<double>(), humidity = data["humidity_pct"].get<double>();
  return std::isfinite(temperature) && std::isfinite(humidity) &&
         temperature >= 0 && temperature <= 50 && humidity >= 0 && humidity <= 100;
}

std::string key(const std::string &session, uint32_t seq) { return session + ":" + std::to_string(seq); }
std::string address(const sockaddr_in &peer) {
  char ip[INET_ADDRSTRLEN]{};
  InetNtopA(AF_INET, &peer.sin_addr, ip, sizeof(ip));
  return std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));
}

struct Route { sockaddr_in peer; Clock::time_point expires; };
struct DelayedAck { sockaddr_in peer; std::string bytes; };

int run(const Options &opt) {
  Wsa wsa;
  Socket sock;
  if (sock.value == INVALID_SOCKET) throw std::runtime_error("Cannot create UDP socket");
  const BOOL exclusive = TRUE;
  if (setsockopt(sock.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
      reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR)
    throw std::runtime_error("Cannot set exclusive UDP port");
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(static_cast<u_short>(opt.port));
  if (bind(sock.value, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == SOCKET_ERROR)
    throw std::runtime_error("Cannot bind port; stop the other UDP receiver first. Error " + std::to_string(WSAGetLastError()));
  const bool relay = !opt.forwardIp.empty();
  sockaddr_in backend{};
  if (relay) {
    backend.sin_family = AF_INET;
    backend.sin_port = htons(static_cast<u_short>(opt.forwardPort));
    if (InetPtonA(AF_INET, opt.forwardIp.c_str(), &backend.sin_addr) != 1)
      throw std::runtime_error("Forward IP must be numeric IPv4");
    const auto ip = ntohl(backend.sin_addr.s_addr);
    if (ip == 0 || ip >= 0xE0000000UL || ((ip >> 24) == 127 && opt.port == opt.forwardPort))
      throw std::runtime_error("Invalid forward IP or loop back to this port");
  }
  const std::filesystem::path output(opt.output);
  if (std::filesystem::exists(output) && !std::filesystem::is_empty(output))
    throw std::runtime_error("Results folder is not empty. Choose a NEW --out folder.");
  std::filesystem::create_directories(output);
  std::ofstream packets(output / "receiver.csv", std::ios::binary);
  std::ofstream measurements(output / "measurements.csv", std::ios::binary);
  std::ofstream events(output / "receiver_events.jsonl", std::ios::binary);
  if (!packets || !measurements || !events) throw std::runtime_error("Cannot open result files");
  const std::string header = "recv_epoch_ms,recv_elapsed_ms,session,seq,sample_uptime_ms,temperature_c,humidity_pct,sensor_valid,duplicate,ack_requested\n";
  packets << "\xEF\xBB\xBF" << header;
  measurements << "\xEF\xBB\xBF" << header;
  Json summary = {{"role", relay ? "relay" : "receiver"}, {"start_epoch_ms", epochMs()},
      {"raw_datagrams", 0}, {"unique", 0}, {"duplicates", 0}, {"invalid", 0},
      {"sensor_errors", 0}, {"dropped_data", 0}, {"dropped_acks", 0},
      {"ack_sent_local", 0}, {"forwarded_data_local", 0}, {"forwarded_acks_local", 0},
      {"drop_ack_every", opt.dropAckEvery}, {"drop_data_every", opt.dropDataEvery},
      {"drop_ack_all", opt.dropAllAcks}, {"ack_delay_ms", opt.ackDelayMs}};
  auto increment = [&](const char *field) { summary[field] = summary[field].get<uint64_t>() + 1; };
  {
    std::ofstream metadata(output / "receiver_metadata.json");
    metadata << summary.dump(2) << '\n';
    if (!metadata) throw std::runtime_error("Cannot write run metadata");
  }
  auto event = [&](const char *kind, const std::string &id, const std::string &extra = "") {
    events << Json({{"epoch_ms", epochMs()}, {"event", kind}, {"key", id}, {"detail", extra}}).dump() << '\n';
    events.flush();
    if (!events) throw std::runtime_error("Event log write failed");
  };
  std::unordered_map<std::string, std::string> seen;
  std::unordered_set<std::string> dataDroppedOnce;
  std::unordered_map<std::string, Route> routes;
  std::multimap<Clock::time_point, DelayedAck> delayed;
  const auto started = Clock::now();
  auto lastValid = started;
  bool stale = false;
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  std::cout << "READY port=" << opt.port << " role=" << (relay ? "relay" : "receiver")
            << " results=" << opt.output << '\n' << std::flush;
  while (running && (!opt.duration || Clock::now() - started < std::chrono::seconds(opt.duration))) {
    const auto now = Clock::now();
    while (!delayed.empty() && delayed.begin()->first <= now) {
      auto entry = delayed.begin();
      const auto &item = entry->second;
      if (sendto(sock.value, item.bytes.data(), int(item.bytes.size()), 0,
          reinterpret_cast<const sockaddr *>(&item.peer), sizeof(item.peer)) == int(item.bytes.size()))
        increment("ack_sent_local");
      else event("ACK_SEND_ERROR", "", std::to_string(WSAGetLastError()));
      delayed.erase(entry);
    }
    for (auto it = routes.begin(); it != routes.end();) {
      if (it->second.expires < now) it = routes.erase(it); else ++it;
    }
    if (!stale && now - lastValid >= std::chrono::seconds(8)) {
      std::cout << "CHƯA CÓ DỮ LIỆU HỢP LỆ TRONG 8 GIÂY\n" << std::flush;
      stale = true;
      event("STALE", "");
    }
    fd_set ready;
    FD_ZERO(&ready); FD_SET(sock.value, &ready);
    timeval wait{0, 20000};
    const int selected = select(0, &ready, nullptr, nullptr, &wait);
    if (selected == SOCKET_ERROR) { if (!running) break; throw std::runtime_error("select failed"); }
    if (!selected) continue;
    char buffer[2048];
    sockaddr_in peer{};
    int peerSize = sizeof(peer);
    const int length = recvfrom(sock.value, buffer, sizeof(buffer), 0,
        reinterpret_cast<sockaddr *>(&peer), &peerSize);
    if (length == SOCKET_ERROR) { increment("invalid"); continue; }
    increment("raw_datagrams");
    const std::string payload(buffer, length);
    char ackSession[17]; uint32_t ackSeq = 0;
    if (ack::parse(buffer, size_t(length), ackSession, ackSeq)) {
      const auto id = key(ackSession, ackSeq);
      const auto route = routes.find(id);
      if (relay && peer.sin_addr.s_addr == backend.sin_addr.s_addr && peer.sin_port == backend.sin_port && route != routes.end()) {
        if (sendto(sock.value, buffer, length, 0, reinterpret_cast<const sockaddr *>(&route->second.peer), sizeof(peer)) == length) {
          increment("forwarded_acks_local");
          event("FORWARD_ACK", id);
        }
      } else event("IGNORED_ACK", id);
      continue;
    }
    const auto data = Json::parse(payload, nullptr, false);
    if (!validData(data)) { increment("invalid"); event("INVALID", "", payload); continue; }
    const auto session = data["session"].get<std::string>();
    const auto seq = data["seq"].get<uint32_t>();
    const auto id = key(session, seq);
    if (opt.dropDataEvery && seq % opt.dropDataEvery == 0 && dataDroppedOnce.insert(id).second) {
      increment("dropped_data"); event("DROP_DATA", id); continue;
    }
    const auto previous = seen.find(id);
    const bool duplicate = previous != seen.end();
    const std::string canonical = data.dump();
    if (duplicate && previous->second != canonical) {
      increment("invalid"); event("CONFLICTING_DUPLICATE", id); continue;
    }
    const bool sensorValid = data["sensor_valid"].get<bool>();
    const bool requested = data.value("ack_required", false);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    std::ostringstream row;
    row << epochMs() << ',' << elapsed << ',' << session << ',' << seq << ','
        << data["uptime_ms"].get<uint32_t>() << ',';
    if (sensorValid) row << data["temperature_c"].get<double>();
    row << ',';
    if (sensorValid) row << data["humidity_pct"].get<double>();
    row << ',' << sensorValid << ',' << duplicate << ',' << requested << '\n';
    packets << row.str(); packets.flush();
    if (!duplicate) {
      measurements << row.str(); measurements.flush();
      seen.emplace(id, canonical);
      increment("unique");
      if (!sensorValid) increment("sensor_errors");
    } else increment("duplicates");
    if (!packets || !measurements) throw std::runtime_error("CSV write failed; no ACK sent");
    lastValid = Clock::now(); stale = false;
    std::cout << (duplicate ? "BẢN TRÙNG" : "SỐ ĐO MỚI") << " | seq=" << seq << " | " << address(peer)
              << " | Nhiệt độ: " << (sensorValid ? data["temperature_c"].dump() + " C" : "--")
              << " | Độ ẩm: " << (sensorValid ? data["humidity_pct"].dump() + " %RH" : "--") << '\n';
    event(duplicate ? "DUPLICATE" : "ACCEPTED", id);
    if (relay) {
      routes[id] = {peer, Clock::now() + std::chrono::seconds(120)};
      if (sendto(sock.value, buffer, length, 0, reinterpret_cast<const sockaddr *>(&backend), sizeof(backend)) == length)
        increment("forwarded_data_local");
      else event("FORWARD_ERROR", id, std::to_string(WSAGetLastError()));
      // Relay chi chuyen ACK tu Khoa, KHONG tu xac nhan thay Khoa.
    } else if (requested) {
      if (opt.dropAllAcks || (!duplicate && opt.dropAckEvery && seq % opt.dropAckEvery == 0)) {
        increment("dropped_acks"); event("DROP_ACK", id);
      } else {
        char reply[40]; const int size = ack::format(reply, sizeof(reply), session.c_str(), seq);
        delayed.emplace(Clock::now() + std::chrono::milliseconds(opt.ackDelayMs), DelayedAck{peer, std::string(reply, size)});
      }
    }
    std::cout.flush();
  }
  summary["end_epoch_ms"] = epochMs();
  summary["pending_delayed_acks"] = delayed.size();
  std::ofstream snapshot(output / "receiver_summary.json");
  snapshot << summary.dump(2) << '\n';
  if (!snapshot) throw std::runtime_error("Cannot write final summary");
  std::cout << "DONE unique=" << summary["unique"] << " duplicates=" << summary["duplicates"]
            << " ack_sent_local=" << summary["ack_sent_local"] << '\n';
  return 0;
}

int main(int argc, char **argv) {
  SetConsoleOutputCP(CP_UTF8);
  try { return run(options(argc, argv)); }
  catch (const std::exception &error) { std::cerr << "ERROR: " << error.what() << '\n'; return 1; }
}
