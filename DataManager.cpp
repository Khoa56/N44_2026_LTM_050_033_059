#include "DataManager.hpp"

#include <stdexcept>
#include <utility>

DataManager::DataManager(std::chrono::milliseconds sensorTimeout, std::chrono::milliseconds telemetryTimeout)
    : sensorTimeout_(sensorTimeout), telemetryTimeout_(telemetryTimeout) {}

void DataManager::publishUdp(const NetworkStats& stats, const std::optional<SensorPacket>& packet,
                             bool advancesSequence, SteadyClock::time_point received,
                             WallClock::time_point timestamp) {
    const std::lock_guard<std::mutex> lock(mutex_);
    packets_ = stats;
    if (!packet) {
        return;
    }
    if (advancesSequence) {
        sensor_ = Timed<SensorPacket>{*packet, received};
    }

    // Giới hạn bộ nhớ khi đĩa hoặc terminal chậm. Báo lỗi rõ ràng nếu hàng đợi đầy.
    constexpr std::size_t maxPendingRows = 8192;
    if (pendingCsv_.size() >= maxPendingRows) {
        throw std::runtime_error("Hang doi CSV day. Dung thu de tranh mat dong am tham.");
    }
    const auto type = advancesSequence ? RecordType::Udp : RecordType::UdpLate;
    const auto status = packet->valid ? SensorStatus::Fresh : SensorStatus::Invalid;
    pendingCsv_.push_back({timestamp, type, *packet, status, uavViewLocked(SteadyClock::now())});
}

DataManager::UavSession DataManager::beginUavSession() {
    const std::lock_guard<std::mutex> lock(mutex_);
    clearTelemetryLocked();
    uavLink_ = UavLink::Connected;
    uavSessionStarted_ = SteadyClock::now();
    return ++uavSession_;
}

void DataManager::endUavSession(UavSession session) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (activeLocked(session)) {
        uavLink_ = UavLink::Reconnecting;
        clearTelemetryLocked();
    }
}

bool DataManager::isUavSessionActive(UavSession session) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return activeLocked(session);
}

void DataManager::setUavConnectionError(std::string error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    uavConnectionError_ = std::move(error);
}

void DataManager::updateFlightMode(UavSession session, std::optional<std::string> mode) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (activeLocked(session)) {
        flightMode_ = Timed<std::optional<std::string>>{std::move(mode), SteadyClock::now()};
    }
}

void DataManager::updateInAir(UavSession session, std::optional<bool> inAir) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (activeLocked(session)) {
        inAir_ = Timed<std::optional<bool>>{inAir, SteadyClock::now()};
    }
}

void DataManager::updatePosition(UavSession session, UavPosition position) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (activeLocked(session)) {
        position_ = Timed<UavPosition>{std::move(position), SteadyClock::now()};
    }
}

Snapshot DataManager::snapshot() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto now = SteadyClock::now();
    auto result = Snapshot{SensorStatus::Waiting, std::nullopt, packets_,
                           uavViewLocked(now), uavConnectionError_};
    const auto lastAdvance = sensor_ ? sensor_->timestamp : started_;
    if (now - lastAdvance > sensorTimeout_) {
        result.sensorStatus = SensorStatus::Stale;
    } else if (sensor_) {
        result.sensor = sensor_->value;
        result.sensorStatus = sensor_->value.valid ? SensorStatus::Fresh : SensorStatus::Invalid;
    }
    return result;
}

std::deque<CsvRecord> DataManager::drainCsv() {
    std::deque<CsvRecord> batch;
    const std::lock_guard<std::mutex> lock(mutex_);
    // Đổi hàng đợi trong mutex, rồi ghi đĩa bên ngoài mutex.
    batch.swap(pendingCsv_);
    return batch;
}

bool DataManager::activeLocked(UavSession session) const {
    return uavLink_ == UavLink::Connected && session == uavSession_;
}

UavState DataManager::uavViewLocked(SteadyClock::time_point now) const {
    UavState result;
    result.link = uavLink_;
    result.telemetryStale = uavLink_ == UavLink::Reconnecting;
    if (uavLink_ != UavLink::Connected) {
        return result;
    }

    const auto fresh = [this, now](const auto& field) {
        return field && now - field->timestamp <= telemetryTimeout_;
    };
    const auto stale = [this, now](const auto& field) {
        return now - (field ? field->timestamp : uavSessionStarted_) > telemetryTimeout_;
    };
    if (fresh(flightMode_)) {
        result.flightMode = flightMode_->value;
    }
    if (fresh(inAir_)) {
        result.inAir = inAir_->value;
    }
    if (fresh(position_)) {
        result.position = position_->value;
    }
    result.telemetryStale = stale(flightMode_) || stale(inAir_) || stale(position_);
    return result;
}

void DataManager::clearTelemetryLocked() {
    flightMode_.reset();
    inAir_.reset();
    position_.reset();
}
