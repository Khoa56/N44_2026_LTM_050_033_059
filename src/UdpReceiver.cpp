#include "UdpReceiver.hpp"

#include <ack_protocol.h>
#include <json.hpp>

#include <algorithm>
#include <array>
#include <bitset>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <cerrno>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
constexpr int maxPayload = 256;

std::optional<double> parseNumber(std::string_view input) {
    double value{};
    const auto end = input.data() + input.size();
    const auto result = std::from_chars(input.data(), end, value, std::chars_format::general);
    if (result.ec == std::errc{} && result.ptr == end && std::isfinite(value)) {
        return value;
    }
    return std::nullopt;
}

bool missingToken(std::string_view input) {
    return input == "nan" || input == "-nan" || input == "N/A";
}

std::optional<SensorPacket> parsePacket(std::string_view payload) {
    if (!payload.empty() && payload.back() == '\n') {
        payload.remove_suffix(1);
        if (!payload.empty() && payload.back() == '\r') {
            payload.remove_suffix(1);
        }
    }

    // Mỗi datagram là một bản tin. Kiểm tra đủ trường, đúng thứ tự và hết chuỗi.
    constexpr std::array<std::string_view, 4> keys{"SEQ:", "TEMP:", "HUM:", "VALID:"};
    std::array<std::string_view, keys.size()> values{};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto separator = payload.find(';');
        const bool last = i + 1 == keys.size();
        const auto field = payload.substr(0, separator);
        if (field.size() <= keys[i].size() || field.substr(0, keys[i].size()) != keys[i] ||
            last != (separator == std::string_view::npos)) {
            return std::nullopt;
        }
        values[i] = field.substr(keys[i].size());
        if (!last) {
            payload.remove_prefix(separator + 1);
        }
    }

    std::uint32_t seq{};
    const auto end = values[0].data() + values[0].size();
    const auto parsed = std::from_chars(values[0].data(), end, seq);
    if (parsed.ec != std::errc{} || parsed.ptr != end ||
        (values[3] != "0" && values[3] != "1")) {
        return std::nullopt;
    }

    const auto temperature = parseNumber(values[1]);
    const auto humidity = parseNumber(values[2]);
    if (values[3] == "1") {
        if (!temperature || !humidity || *humidity < 0.0 || *humidity > 100.0) {
            return std::nullopt;
        }
        return SensorPacket{seq, temperature, humidity, true, {}, {}};
    }

    // VALID:0 báo lần đọc lỗi. ESP32 có thể vẫn gửi giá trị thô hoặc nan.
    if ((!temperature && !missingToken(values[1])) || (!humidity && !missingToken(values[2]))) {
        return std::nullopt;
    }
    return SensorPacket{seq, std::nullopt, std::nullopt, false, {}, {}};
}

using Json = nlohmann::json;

std::optional<std::uint32_t> jsonUint32(const Json& value) {
    if (!value.is_number_unsigned() ||
        value.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return value.get<std::uint32_t>();
}

std::optional<SensorPacket> parseJsonPacket(std::string_view payload, bool& requiresAck) {
    // NUL không hợp lệ trên dây. Một số bộ đọc JSON coi nó như cuối luồng.
    if (payload.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto body = Json::parse(payload.begin(), payload.end(), nullptr, false);
    if (!body.is_object()) {
        return std::nullopt;
    }
    for (const char* field : {"session", "seq", "uptime_ms", "temperature_c",
                              "humidity_pct", "sensor_valid"}) {
        if (!body.contains(field)) {
            return std::nullopt;
        }
    }
    if ((body.contains("type") && body["type"] != "data") ||
        (body.contains("ack_required") && !body["ack_required"].is_boolean()) ||
        !body["session"].is_string() || !body["sensor_valid"].is_boolean()) {
        return std::nullopt;
    }
    auto session = body["session"].get<std::string>();
    const auto seq = jsonUint32(body["seq"]);
    const auto uptime = jsonUint32(body["uptime_ms"]);
    if (session.size() != 16 || !ack::validSession(session.c_str()) || !seq || !uptime) {
        return std::nullopt;
    }

    const auto& temperature = body["temperature_c"];
    const auto& humidity = body["humidity_pct"];
    const bool valid = body["sensor_valid"].get<bool>();
    SensorPacket packet{*seq, {}, {}, valid, std::move(session), uptime};
    if (valid) {
        if (!temperature.is_number() || !humidity.is_number()) {
            return std::nullopt;
        }
        const double t = temperature.get<double>();
        const double h = humidity.get<double>();
        if (!std::isfinite(t) || !std::isfinite(h) || h < 0.0 || h > 100.0) {
            return std::nullopt;
        }
        packet.temperatureC = t;
        packet.humidityPct = h;
    } else if (!temperature.is_null() || !humidity.is_null()) {
        return std::nullopt;
    }
    requiresAck = body.value("ack_required", false);
    return packet;
}

enum class SequenceResult { Forward, Recovered, Duplicate, TooOld };

class SequenceWindow {
public:
    SequenceResult observe(std::uint32_t seq, NetworkStats& stats) {
        if (!head_) {
            head_ = seq;
            seen_.set(0);
            depth_ = 1;
            return SequenceResult::Forward;
        }

        // Phép trừ modulo 2^32 cho phép UINT32_MAX -> 0. Nửa vòng là mơ hồ.
        const std::uint32_t forward = seq - *head_;
        if (forward != 0 && forward < (std::uint32_t{1} << 31)) {
            if (forward > 1) {
                stats.missing += forward - 1;
                stats.lastGap = SequenceGap{*head_ + 1, seq - 1};
            }
            seen_ <<= forward;
            seen_.set(0);
            depth_ = std::min(windowSize, depth_ + forward);
            head_ = seq;
            return SequenceResult::Forward;
        }

        const std::uint32_t age = *head_ - seq;
        if (age >= depth_) {
            ++stats.tooOld;
            return SequenceResult::TooOld;
        }
        if (seen_.test(age)) {
            ++stats.duplicates;
            return SequenceResult::Duplicate;
        }

        // Bit chưa được đặt trong phạm vi quan sát tương ứng đúng một seq đang thiếu.
        seen_.set(age);
        --stats.missing;
        ++stats.recovered;
        return SequenceResult::Recovered;
    }

private:
    static constexpr std::size_t windowSize = 1024;
    std::optional<std::uint32_t> head_;
    std::bitset<windowSize> seen_;
    std::size_t depth_ = 0;
};

class SensorStream {
public:
    SequenceResult observe(const SensorPacket& packet, NetworkStats& stats) {
        if (packet.session != currentBoot_) {
            if (!packet.session ||
                std::find(recentBoots_.begin(), recentBoots_.end(), *packet.session) != recentBoots_.end()) {
                ++stats.tooOld;
                return SequenceResult::TooOld;
            }
            if (recentBoots_.size() == 64) {
                recentBoots_.pop_front();
            }
            currentBoot_ = packet.session;
            recentBoots_.push_back(*currentBoot_);
            sequences_ = {};
            stats.lastGap.reset();
            ++stats.sensorBoots;
        }
        return sequences_.observe(packet.seq, stats);
    }

private:
    SequenceWindow sequences_;
    std::optional<std::string> currentBoot_;
    std::deque<std::string> recentBoots_;
};

#ifdef _WIN32
using SocketHandle = SOCKET;
using SocketAddressLength = int;
constexpr auto invalidSocket = INVALID_SOCKET;
int socketError() { return WSAGetLastError(); }
bool retryable(int error) {
    // Một ACK gửi tới cổng đã đóng có thể khiến recvfrom báo ICMP Port Unreachable.
    return error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSAECONNRESET;
}

class Winsock {
public:
    Winsock() {
        WSADATA data{};
        const int error = WSAStartup(MAKEWORD(2, 2), &data);
        if (error != 0) {
            throw std::runtime_error("WSAStartup: " + std::to_string(error));
        }
    }
    ~Winsock() { WSACleanup(); }
    Winsock(const Winsock&) = delete;
    Winsock& operator=(const Winsock&) = delete;
};
#else
using SocketHandle = int;
using SocketAddressLength = socklen_t;
constexpr auto invalidSocket = -1;
int socketError() { return errno; }
bool retryable(int error) { return error == EINTR || error == EAGAIN || error == EWOULDBLOCK; }
#endif

class UdpSocket {
public:
    UdpSocket() : handle_(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) {
        if (handle_ == invalidSocket) {
            throw std::runtime_error("socket: " + std::to_string(socketError()));
        }
    }
    ~UdpSocket() {
#ifdef _WIN32
        closesocket(handle_);
#else
        ::close(handle_);
#endif
    }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    SocketHandle get() const { return handle_; }

private:
    SocketHandle handle_;
};

int waitReadable(SocketHandle socket) {
    // Thời gian chờ hữu hạn để luồng luôn kiểm tra cờ dừng khi không có UDP.
#ifdef _WIN32
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(socket, &ready);
    timeval timeout{0, 100000};
    return ::select(0, &ready, nullptr, nullptr, &timeout);
#else
    pollfd ready{socket, POLLIN, 0};
    const int result = ::poll(&ready, 1, 100);
    if (result > 0 && !(ready.revents & POLLIN)) {
        throw std::runtime_error("poll: socket UDP khong doc duoc");
    }
    return result;
#endif
}
} // namespace

UdpReceiver::UdpReceiver(DataManager& data, std::uint16_t port) : data_(data), port_(port) {}

void UdpReceiver::run(const std::atomic<bool>& stop) {
#ifdef _WIN32
    const Winsock winsock;
#endif
    const UdpSocket socket;
#ifdef _WIN32
    const BOOL exclusive = TRUE;
    if (::setsockopt(socket.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0) {
        throw std::runtime_error("SO_EXCLUSIVEADDRUSE: " + std::to_string(socketError()));
    }
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address),
               static_cast<int>(sizeof(address))) != 0) {
        throw std::runtime_error("Bind UDP port " + std::to_string(port_) + " (OS error " +
                                 std::to_string(socketError()) + ")");
    }

    // Đọc dư một byte để phân biệt gói hợp lệ 256 byte với datagram bị cắt trên POSIX.
    std::array<char, maxPayload + 1> buffer{};
    NetworkStats stats;
    SensorStream stream;
    while (!stop.load()) {
        const int ready = waitReadable(socket.get());
        if (ready < 0) {
            const int error = socketError();
            if (retryable(error)) {
                continue;
            }
            throw std::runtime_error("Cho UDP: " + std::to_string(error));
        }
        if (ready == 0 || stop.load()) {
            continue;
        }

        sockaddr_in peer{};
        SocketAddressLength peerLength = sizeof(peer);
        const auto length = ::recvfrom(socket.get(), buffer.data(), static_cast<int>(buffer.size()),
                                       0, reinterpret_cast<sockaddr*>(&peer), &peerLength);
        if (length < 0) {
            const int error = socketError();
            if (retryable(error)) {
                continue;
            }
#ifdef _WIN32
            // Winsock trả WSAEMSGSIZE và loại phần dư của datagram UDP.
            if (error == WSAEMSGSIZE) {
                ++stats.total;
                ++stats.malformed;
                data_.publishUdp(stats, std::nullopt, false, {}, {});
                continue;
            }
#endif
            throw std::runtime_error("recvfrom: " + std::to_string(error));
        }

        const auto received = SteadyClock::now();
        const auto timestamp = WallClock::now();
        ++stats.total;
        std::optional<SensorPacket> packet;
        bool requiresAck = false;
        if (length <= maxPayload) {
            const std::string_view payload(buffer.data(), static_cast<std::size_t>(length));
            const auto first = payload.find_first_not_of(" \t\r\n");
            packet = first != std::string_view::npos && payload[first] == '{'
                ? parseJsonPacket(payload, requiresAck) : parsePacket(payload);
        }

        bool advances = false;
        if (!packet) {
            ++stats.malformed;
        } else {
            const auto sequence = stream.observe(*packet, stats);
            // Gửi từ chính cổng nhận. Gửi lại ACK cho bản tin trùng khi ACK trước bị mất.
            if (requiresAck && sequence != SequenceResult::TooOld) {
                std::array<char, 32> reply{};
                const auto replyLength = ack::format(reply.data(), reply.size(),
                                                     packet->session->c_str(), packet->seq);
                const auto sent = ::sendto(socket.get(), reply.data(), replyLength, 0,
                                           reinterpret_cast<const sockaddr*>(&peer), peerLength);
                if (sent == replyLength) {
                    ++stats.acksSent;
                } else {
                    ++stats.ackErrors;
                }
            }
            advances = sequence == SequenceResult::Forward;
            if (advances || sequence == SequenceResult::Recovered) {
                ++stats.unique;
                if (!packet->valid) {
                    ++stats.invalidSensor;
                }
            } else {
                packet.reset();
            }
        }
        data_.publishUdp(stats, packet, advances, received, timestamp);
    }
}
