#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

using SteadyClock = std::chrono::steady_clock;
using WallClock = std::chrono::system_clock;

// optional phân biệt số đo chưa có với giá trị 0 hợp lệ.
struct SensorPacket {
    std::uint32_t seq;
    std::optional<double> temperatureC;
    std::optional<double> humidityPct;
    bool valid;
    std::optional<std::string> session;
    std::optional<std::uint32_t> uptimeMs;
};

struct SequenceGap {
    std::uint32_t first;
    std::uint32_t last;
};

struct NetworkStats {
    std::uint64_t total = 0;
    std::uint64_t malformed = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t tooOld = 0;
    std::uint64_t unique = 0;
    std::uint64_t invalidSensor = 0;
    std::uint64_t recovered = 0;
    std::uint64_t missing = 0;
    std::optional<SequenceGap> lastGap;
    std::uint64_t acksSent = 0;
    std::uint64_t ackErrors = 0;
    std::uint64_t sensorBoots = 0;
};

struct UavPosition {
    std::optional<double> latitudeDeg;
    std::optional<double> longitudeDeg;
    std::optional<double> relativeAltitudeM;
};

enum class SensorStatus { Waiting, Fresh, Invalid, Stale };
enum class UavLink { Waiting, Connected, Reconnecting };
enum class RecordType { Udp, UdpLate, Status };

struct UavState {
    UavLink link = UavLink::Waiting;
    std::optional<std::string> flightMode;
    std::optional<bool> inAir;
    UavPosition position;
    bool telemetryStale = false;
};

struct Snapshot {
    SensorStatus sensorStatus;
    std::optional<SensorPacket> sensor;
    NetworkStats packets;
    UavState uav;
    std::string uavConnectionError;
};

struct CsvRecord {
    WallClock::time_point timestamp;
    RecordType type;
    std::optional<SensorPacket> sensor;
    SensorStatus sensorStatus;
    UavState uav;
};

inline const char* label(SensorStatus status) noexcept {
    switch (status) {
        case SensorStatus::Waiting: return "WAITING";
        case SensorStatus::Fresh: return "FRESH";
        case SensorStatus::Invalid: return "INVALID";
        case SensorStatus::Stale: return "STALE";
    }
    return "N/A";
}

inline const char* label(UavLink link) noexcept {
    switch (link) {
        case UavLink::Waiting: return "WAITING";
        case UavLink::Connected: return "CONNECTED";
        case UavLink::Reconnecting: return "RECONNECTING";
    }
    return "N/A";
}

inline const char* label(RecordType type) noexcept {
    switch (type) {
        case RecordType::Udp: return "UDP";
        case RecordType::UdpLate: return "UDP_LATE";
        case RecordType::Status: return "STATUS";
    }
    return "N/A";
}
