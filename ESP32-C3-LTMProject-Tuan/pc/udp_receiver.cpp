// Windows C++17: nhan UDP va tuy chon chuyen tiep nguyen ban tin.
// Build MinGW: g++ -std=c++17 -O2 -static udp_receiver.cpp -o udp_receiver_readable.exe -lws2_32
// Build MSVC: cl /EHsc /std:c++17 udp_receiver.cpp ws2_32.lib
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <chrono>
#include <cmath>
#include <limits>
#include "third_party/json.hpp"
#include <csignal>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

volatile std::sig_atomic_t running = 1;
void stop(int) { running = 0; }

unsigned short parsePort(const char *text) {
  const std::string value(text);
  if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
    throw std::runtime_error("Port must be a number in 1..65535.");
  const unsigned long number = std::stoul(value);
  if (number < 1 || number > 65535) throw std::runtime_error("Port must be in 1..65535.");
  return static_cast<unsigned short>(number);
}


// Kiem tra JSON truoc khi hien thi; khong bien du lieu loi thanh so 0.
bool validSensorPacket(const nlohmann::json &data) {
  if (!data.is_object()) return false;
  for (const char *key : {"session", "seq", "uptime_ms", "temperature_c", "humidity_pct", "sensor_valid"}) {
    if (!data.contains(key)) return false;
  }
  if (!data["session"].is_string() || !data["seq"].is_number_unsigned() ||
      !data["uptime_ms"].is_number_unsigned() || !data["sensor_valid"].is_boolean()) return false;
  const std::string session = data["session"].get<std::string>();
  if (session.size() != 16 || session.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    return false;
  const auto maxCounter = std::numeric_limits<std::uint32_t>::max();
  if (data["seq"].get<std::uint64_t>() > maxCounter ||
      data["uptime_ms"].get<std::uint64_t>() > maxCounter) return false;
  if (!data["sensor_valid"].get<bool>())
    return data["temperature_c"].is_null() && data["humidity_pct"].is_null();
  if (!data["temperature_c"].is_number() || !data["humidity_pct"].is_number()) return false;
  const double temperature = data["temperature_c"].get<double>();
  const double humidity = data["humidity_pct"].get<double>();
  return std::isfinite(temperature) && std::isfinite(humidity) &&
         temperature >= 0 && temperature <= 50 && humidity >= 0 && humidity <= 100;
}

// Chuyen cac truong JSON thanh thong tin nguoi doc hieu duoc.
bool displaySensorData(const std::string &payload, const char *sourceIp,
                       unsigned short sourcePort, std::uint64_t received) {
  const auto data = nlohmann::json::parse(payload, nullptr, false);
  if (!validSensorPacket(data)) {
    std::cout << "\nBản tin #" << received << " từ " << sourceIp
              << ": sai định dạng hoặc giá trị không hợp lệ. Đã lưu bản gốc vào log.\n";
    return false;
  }
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  std::cout << "\n[" << std::put_time(&local, "%H:%M:%S") << "] ĐANG NHẬN DỮ LIỆU"
            << " | RX=" << received << " | " << sourceIp << ':' << sourcePort << '\n';
  if (data["sensor_valid"].get<bool>()) {
    std::cout << std::fixed << std::setprecision(2)
              << "  Nhiệt độ : " << data["temperature_c"].get<double>() << " °C\n"
              << "  Độ ẩm    : " << data["humidity_pct"].get<double>() << " %RH\n"
              << "  Cảm biến : HỢP LỆ\n";
  } else {
    std::cout << "  Nhiệt độ : --\n  Độ ẩm    : --\n"
              << "  Cảm biến : LỖI ĐỌC; ESP32 vẫn gửi được bản tin\n";
  }
  std::cout << "  Số thứ tự: " << data["seq"].get<std::uint32_t>()
            << " | Phiên: " << data["session"].get<std::string>() << '\n';
  return true;
}

int main(int argc, char **argv) {
  SetConsoleOutputCP(CP_UTF8);
  if (argc != 2 && argc != 4) {
    std::cerr << "Receive: udp_receiver_readable.exe LISTEN_PORT\n"
              << "Relay:   udp_receiver_readable.exe LISTEN_PORT FORWARD_IPV4 FORWARD_PORT\n"
              << "Example: udp_receiver_readable.exe 5005 26.10.20.30 5005\n";
    return 1;
  }
  unsigned short listenPort;
  sockaddr_in destination{};
  const bool relay = argc == 4;
  try {
    listenPort = parsePort(argv[1]);
    if (relay) {
      destination.sin_family = AF_INET;
      destination.sin_port = htons(parsePort(argv[3]));
      if (InetPtonA(AF_INET, argv[2], &destination.sin_addr) != 1)
        throw std::runtime_error("Forward address must be a numeric IPv4 address.");
      const auto ip = ntohl(destination.sin_addr.s_addr);
      if (ip == 0 || ip >= 0xE0000000UL)
        throw std::runtime_error("Forward address must be a unicast IPv4 address.");
      if ((ip >> 24) == 127 && destination.sin_port == htons(listenPort))
        throw std::runtime_error("Do not forward to your own listening port.");
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    std::cerr << "WSAStartup failed.\n";
    return 1;
  }
  const SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == INVALID_SOCKET) {
    std::cerr << "socket error=" << WSAGetLastError() << '\n';
    WSACleanup();
    return 1;
  }
  // Khong cho phep hai tien trinh cung nhan mot cong tren may nay.
  const BOOL exclusive = TRUE;
  if (setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR) {
    std::cerr << "setsockopt error=" << WSAGetLastError() << '\n';
    closesocket(sock);
    WSACleanup();
    return 1;
  }
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0: nhan tu LAN hoac VPN.
  local.sin_port = htons(listenPort);
  if (bind(sock, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == SOCKET_ERROR) {
    std::cerr << "bind error=" << WSAGetLastError()
              << " (check whether this port is already in use).\n";
    closesocket(sock);
    WSACleanup();
    return 1;
  }
  // Socket rieng de gui: tranh dung socket da bind INADDR_ANY cho ca hai vai tro.
  const SOCKET outgoing = relay ? socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : INVALID_SOCKET;
  if (relay && outgoing == INVALID_SOCKET) {
    std::cerr << "Cannot create forwarding socket.\n";
    closesocket(sock);
    WSACleanup();
    return 1;
  }
  std::ofstream log("udp_received.log", std::ios::app | std::ios::binary);
  if (!log) {
    std::cerr << "Cannot open udp_received.log in this directory.\n";
    if (relay) closesocket(outgoing);
    closesocket(sock);
    WSACleanup();
    return 1;
  }
  std::signal(SIGINT, stop);
  std::cout << "Listening on 0.0.0.0:" << listenPort << "; Ctrl+C to stop.\n";
  if (relay) std::cout << "Forwarding unchanged to " << argv[2] << ':' << argv[3] << '\n';
  std::cout << "Hiển thị số đo đã đọc từ JSON. Bản tin gốc được lưu tại udp_received.log. RX đếm datagram nhận.\n";
  std::cout.flush();
  std::uint64_t received = 0, forwarded = 0;
  auto lastReceived = std::chrono::steady_clock::now();
  bool staleShown = false;
  int exitCode = 0;
  while (running) {
    if (!staleShown && std::chrono::steady_clock::now() - lastReceived >= std::chrono::seconds(8)) {
      std::cout << "\nCHƯA CÓ BẢN TIN HỢP LỆ TRONG 8 GIÂY. Kiểm tra ESP32/Wi-Fi/VPN.\n" << std::flush;
      staleShown = true;
    }
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(sock, &ready);
    timeval timeout{1, 0};
    const int status = select(0, &ready, nullptr, nullptr, &timeout);
    if (status == SOCKET_ERROR) {
      if (running) std::cerr << "select error=" << WSAGetLastError() << '\n';
      exitCode = running ? 1 : 0;
      break;
    }
    if (status == 0) {
      continue;
    }
    char packet[2048];
    sockaddr_in source{};
    int sourceLength = sizeof(source);
    const int length = recvfrom(sock, packet, sizeof(packet), 0,
        reinterpret_cast<sockaddr *>(&source), &sourceLength);
    if (length == SOCKET_ERROR) {
      std::cerr << "recvfrom error=" << WSAGetLastError() << "; packet discarded.\n";
      continue;
    }
    ++received;
    char sourceIp[INET_ADDRSTRLEN]{};
    InetNtopA(AF_INET, &source.sin_addr, sourceIp, sizeof(sourceIp));
    const std::string payload(packet, length);
    const auto now = std::chrono::system_clock::now();
    const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    // quoted ghi escape dau nhay va dau backslash; van giu ban tin trong payload.
    log << epochMs << '\t' << sourceIp << ':' << ntohs(source.sin_port)
        << '\t' << std::quoted(payload) << '\n';
    log.flush();
    if (!log) {
      std::cerr << "Log write failed.\n";
      exitCode = 1;
      break;
    }
    if (displaySensorData(payload, sourceIp, ntohs(source.sin_port), received)) {
      lastReceived = std::chrono::steady_clock::now();
      staleShown = false;
    }
    if (relay) {
      const int sent = sendto(outgoing, packet, length, 0,
          reinterpret_cast<const sockaddr *>(&destination), sizeof(destination));
      if (sent == length) ++forwarded;
      else std::cerr << "Forward failed; WSA error=" << WSAGetLastError() << '\n';
      std::cout << "Forward local OK=" << forwarded << " (not a remote ACK).\n";
    }
    std::cout.flush();
  }
  if (relay) closesocket(outgoing);
  closesocket(sock);
  WSACleanup();
  return exitCode;
}
