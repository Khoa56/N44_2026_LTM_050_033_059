#pragma once

#include "Common.hpp"

#include <cstddef>
#include <deque>
#include <mutex>

class DataManager {
public:
    using UavSession = std::uint64_t;

    DataManager(std::chrono::milliseconds sensorTimeout, std::chrono::milliseconds telemetryTimeout);

    // Chỉ luồng UDP cập nhật thống kê. Gói đến muộn vẫn có dòng CSV riêng.
    void publishUdp(const NetworkStats& stats, const std::optional<SensorPacket>& packet,
                    bool advancesSequence, SteadyClock::time_point received,
                    WallClock::time_point timestamp);

    UavSession beginUavSession();
    void endUavSession(UavSession session);
    bool isUavSessionActive(UavSession session) const;
    void setUavConnectionError(std::string error);
    void updateFlightMode(UavSession session, std::optional<std::string> mode);
    void updateInAir(UavSession session, std::optional<bool> inAir);
    void updatePosition(UavSession session, UavPosition position);

    Snapshot snapshot() const;
    std::deque<CsvRecord> drainCsv();

private:
    template<class T>
    struct Timed {
        T value;
        SteadyClock::time_point timestamp;
    };

    // Các hàm có hậu tố Locked được gọi khi đã giữ mutex_.
    bool activeLocked(UavSession session) const;
    UavState uavViewLocked(SteadyClock::time_point now) const;
    void clearTelemetryLocked();

    const std::chrono::milliseconds sensorTimeout_;
    const std::chrono::milliseconds telemetryTimeout_;
    const SteadyClock::time_point started_ = SteadyClock::now();
    mutable std::mutex mutex_;
    NetworkStats packets_;
    std::optional<Timed<SensorPacket>> sensor_;
    std::deque<CsvRecord> pendingCsv_;
    UavLink uavLink_ = UavLink::Waiting;
    UavSession uavSession_ = 0;
    SteadyClock::time_point uavSessionStarted_{};
    std::optional<Timed<std::optional<std::string>>> flightMode_;
    std::optional<Timed<std::optional<bool>>> inAir_;
    std::optional<Timed<UavPosition>> position_;
    std::string uavConnectionError_;
};
