#include "UavTelemetry.hpp"

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/telemetry/telemetry.h>

#include <cmath>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>

namespace {
using namespace std::chrono_literals;
using mavsdk::Telemetry;

std::optional<double> finiteValue(double value) {
    if (std::isfinite(value)) {
        return value;
    }
    return std::nullopt;
}

std::optional<bool> toInAir(Telemetry::LandedState state) {
    using State = Telemetry::LandedState;
    switch (state) {
        case State::OnGround: return false;
        case State::TakingOff:
        case State::InAir:
        case State::Landing: return true;
        case State::Unknown: return std::nullopt;
    }
    return std::nullopt;
}

void monitorSession(DataManager& data, const std::shared_ptr<mavsdk::System>& system,
                    Telemetry& telemetry, const std::atomic<bool>& stop) {
    const auto session = data.beginUavSession();

    // MAVSDK gọi callback trên luồng nội bộ. Callback chỉ cập nhật DataManager.
    // Mã phiên khiến callback đã xếp hàng của lần kết nối cũ không thể ghi đè lần mới.
    const auto connectionHandle = system->subscribe_is_connected([&data, session](bool connected) {
        if (!connected) {
            data.endUavSession(session);
        }
    });
    const auto modeHandle = telemetry.subscribe_flight_mode([&data, session](auto mode) {
        if (mode == Telemetry::FlightMode::Unknown) {
            data.updateFlightMode(session, std::nullopt);
        } else {
            std::ostringstream name;
            name << mode;
            data.updateFlightMode(session, name.str());
        }
    });
    // LandedState giữ được trạng thái Unknown. Bool in_air() của SDK có thể trả giá trị cũ
    // nếu bản tin EXTENDED_SYS_STATE không xác định được UAV đang ở đâu.
    const auto landedHandle = telemetry.subscribe_landed_state([&data, session](auto state) {
        data.updateInAir(session, toInAir(state));
    });
    const auto positionHandle = telemetry.subscribe_position([&data, session](auto position) {
        data.updatePosition(session, {finiteValue(position.latitude_deg),
                                      finiteValue(position.longitude_deg),
                                      finiteValue(position.relative_altitude_m)});
    });

    // Kiểm tra trực tiếp trạng thái để xử lý cả khi callback mất kết nối đang chờ chạy.
    while (!stop.load() && system->is_connected() && data.isUavSessionActive(session)) {
        std::this_thread::sleep_for(50ms);
    }
    if (!stop.load()) {
        data.endUavSession(session);
    }
    telemetry.unsubscribe_position(positionHandle);
    telemetry.unsubscribe_landed_state(landedHandle);
    telemetry.unsubscribe_flight_mode(modeHandle);
    system->unsubscribe_is_connected(connectionHandle);
}
} // namespace

UavTelemetry::UavTelemetry(DataManager& data, std::string url, std::chrono::milliseconds timeout)
    : data_(data), url_(std::move(url)), timeout_(timeout) {}

void UavTelemetry::run(const std::atomic<bool>& stop) {
    mavsdk::Mavsdk sdk{mavsdk::Mavsdk::Configuration{mavsdk::ComponentType::CompanionComputer}};
    sdk.set_heartbeat_timeout_s(std::chrono::duration<double>(timeout_).count());

    // Lỗi mở endpoint (ví dụ cổng đang bận) không làm gián đoạn luồng ESP32.
    while (!stop.load()) {
        const auto result = sdk.add_any_connection(url_);
        if (result == mavsdk::ConnectionResult::Success) {
            data_.setUavConnectionError({});
            break;
        }
        std::ostringstream error;
        error << result;
        data_.setUavConnectionError(error.str());
        for (int retry = 0; retry < 20 && !stop.load(); ++retry) {
            std::this_thread::sleep_for(50ms);
        }
    }

    // Theo dõi một autopilot. Giữ System và plugin để MAVSDK tự nhận lại heartbeat.
    std::shared_ptr<mavsdk::System> autopilot;
    while (!stop.load() && !autopilot) {
        for (const auto& system : sdk.systems()) {
            if (system->is_connected() && system->has_autopilot()) {
                autopilot = system;
                break;
            }
        }
        if (!autopilot) {
            std::this_thread::sleep_for(50ms);
        }
    }
    if (!autopilot || stop.load()) {
        return;
    }

    Telemetry telemetry{autopilot};
    while (!stop.load()) {
        if (autopilot->is_connected()) {
            monitorSession(data_, autopilot, telemetry, stop);
        } else {
            std::this_thread::sleep_for(50ms);
        }
    }
}
