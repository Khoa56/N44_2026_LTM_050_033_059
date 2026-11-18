#include "DataManager.hpp"
#include "Logger.hpp"
#include "UavTelemetry.hpp"
#include "UdpReceiver.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <exception>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
using namespace std::chrono_literals;

std::atomic<bool> stopRequested{false};
static_assert(std::atomic<bool>::is_always_lock_free,
              "SIGINT requires a lock-free atomic flag");

void onSigint(int) noexcept {
    // Khóa mutex, in terminal và ghi CSV đều thực hiện sau khi rời signal handler.
    stopRequested.store(true, std::memory_order_relaxed);
}

#ifdef _WIN32
BOOL WINAPI onConsoleControl(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        onSigint(SIGINT);
        return TRUE;
    }
    return FALSE;
}
#endif

struct Options {
    std::uint16_t udpPort = 8080;
    std::string mavsdkUrl = "udpin://0.0.0.0:14600";
    std::filesystem::path csv = "monitor-v3.csv";
    std::chrono::milliseconds sensorTimeout = 5s;
    std::chrono::milliseconds telemetryTimeout = 3s;
    std::chrono::milliseconds refresh = 500ms;
    bool plain = false;
    bool help = false;
};

int parseInt(std::string_view value, int min, int max, std::string_view flag) {
    int number{};
    const auto end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, number);
    if (result.ec != std::errc{} || result.ptr != end || number < min || number > max) {
        throw std::runtime_error("Gia tri sai cho " + std::string(flag));
    }
    return number;
}

Options parseOptions(int argc, char* argv[]) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        if (flag == "--help" || flag == "-h") {
            opts.help = true;
            return opts;
        }
        if (flag == "--plain") {
            opts.plain = true;
            continue;
        }
        if (i + 1 == argc || std::string_view(argv[i + 1]).empty() ||
            std::string_view(argv[i + 1]).substr(0, 2) == "--") {
            throw std::runtime_error("Thieu gia tri hoac tuy chon sai: " + std::string(flag));
        }
        const std::string_view value = argv[++i];
        if (flag == "--udp-port") {
            opts.udpPort = static_cast<std::uint16_t>(parseInt(value, 1, 65535, flag));
        } else if (flag == "--mavsdk-url") {
            if (value.substr(0, 8) != "udpin://" && value.substr(0, 9) != "udpout://") {
                throw std::runtime_error("Dung endpoint MAVSDK udpin://IP:PORT hoac udpout://IP:PORT");
            }
            opts.mavsdkUrl = value;
        } else if (flag == "--csv") {
            opts.csv = value;
        } else if (flag == "--timeout-ms") {
            opts.sensorTimeout = opts.telemetryTimeout =
                std::chrono::milliseconds{parseInt(value, 1000, 600000, flag)};
        } else if (flag == "--sensor-timeout-ms") {
            opts.sensorTimeout = std::chrono::milliseconds{parseInt(value, 1000, 600000, flag)};
        } else if (flag == "--telemetry-timeout-ms") {
            opts.telemetryTimeout = std::chrono::milliseconds{parseInt(value, 1000, 600000, flag)};
        } else if (flag == "--refresh-ms") {
            if (value != "500" && value != "1000") {
                throw std::runtime_error("--refresh-ms nhan 500 hoac 1000");
            }
            opts.refresh = std::chrono::milliseconds{value == "500" ? 500 : 1000};
        } else {
            throw std::runtime_error("Tuy chon khong nhan ra: " + std::string(flag));
        }
    }
    return opts;
}

void printHelp() {
    std::cout << "esp32_px4_monitor [options]\n"
              << "  --udp-port PORT            ESP32 UDP listen port (8080)\n"
              << "  --mavsdk-url URL           PX4 endpoint (udpin://0.0.0.0:14600)\n"
              << "  --csv FILE                 CSV output path (monitor-v3.csv)\n"
              << "  --sensor-timeout-ms MS     Sensor timeout (5000)\n"
              << "  --telemetry-timeout-ms MS  PX4 heartbeat and data timeout (3000)\n"
              << "  --timeout-ms MS            Set both timeouts\n"
              << "  --refresh-ms 500|1000      Dashboard interval (500)\n"
              << "  --plain                    Print snapshots without screen redraw\n"
              << "  --help                     Show this help\n";
}

class Terminal {
public:
    explicit Terminal(bool plain) {
#ifdef _WIN32
        handle_ = GetStdHandle(STD_OUTPUT_HANDLE);
        if (GetConsoleMode(handle_, &originalMode_)) {
            originalCodePage_ = GetConsoleOutputCP();
            SetConsoleOutputCP(CP_UTF8);
            redraw_ = !plain && SetConsoleMode(handle_, originalMode_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
#else
        redraw_ = !plain && ::isatty(STDOUT_FILENO) != 0;
#endif
    }
    ~Terminal() {
#ifdef _WIN32
        if (redraw_) {
            SetConsoleMode(handle_, originalMode_);
        }
        if (originalCodePage_ != 0) {
            SetConsoleOutputCP(originalCodePage_);
        }
#endif
    }
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    void draw(const std::string& text) const {
        std::cout << (redraw_ ? "\x1b[H\x1b[J" : "\n") << text << std::flush;
    }

private:
    bool redraw_ = false;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    DWORD originalMode_ = 0;
    UINT originalCodePage_ = 0;
#endif
};

std::string displayNumber(const std::optional<double>& value, int decimals) {
    if (!value) {
        return "N/A";
    }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(decimals) << *value;
    return out.str();
}

const char* displayBool(const std::optional<bool>& value) {
    return value ? (*value ? "true" : "false") : "N/A";
}

std::string dashboard(const Snapshot& state, const Options& opts, WallClock::time_point now) {
    const auto& sensor = state.sensor;
    const auto& uav = state.uav;
    const auto& packets = state.packets;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "GIÁM SÁT ESP32 VÀ PX4 SITL  |  " << utcTimestamp(now) << '\n'
        << "ESP32 UDP " << opts.udpPort
#if MONITOR_WITH_MAVSDK
        << "  |  PX4 " << opts.mavsdkUrl << '\n'
#else
        << "  |  Bản kiểm thử UDP\n"
#endif
        << "------------------------------------------------------------\n"
        << "CẢM BIẾN     " << label(state.sensorStatus) << '\n'
        << "  Seq " << (sensor ? std::to_string(sensor->seq) : "N/A")
        << "  |  Phiên " << (sensor ? sensor->session.value_or("N/A") : "N/A")
        << "  |  Valid " << displayBool(sensor ? std::optional<bool>{sensor->valid} : std::nullopt) << '\n'
        << "  Nhiệt độ (°C) " << displayNumber(sensor ? sensor->temperatureC : std::nullopt, 2)
        << "  |  Độ ẩm (%RH) " << displayNumber(sensor ? sensor->humidityPct : std::nullopt, 2) << '\n'
        << "UAV          " << label(uav.link) << (uav.telemetryStale ? "  |  TELEMETRY STALE" : "") << '\n'
        << "  Flight mode " << uav.flightMode.value_or("N/A") << "  |  In air " << displayBool(uav.inAir) << '\n'
        << "  Vĩ độ (°) " << displayNumber(uav.position.latitudeDeg, 7)
        << "  |  Kinh độ (°) " << displayNumber(uav.position.longitudeDeg, 7) << '\n'
        << "  Độ cao tương đối (m) " << displayNumber(uav.position.relativeAltitudeM, 2) << '\n';
    if (!state.uavConnectionError.empty()) {
        out << "  Đang thử mở lại kết nối MAVSDK (" << state.uavConnectionError << ")\n";
    }

    out << "UDP          Tổng " << packets.total << "  |  Đúng định dạng, duy nhất " << packets.unique << '\n'
        << "  Sai định dạng " << packets.malformed << "  |  Trùng " << packets.duplicates
        << "  |  Quá cửa sổ " << packets.tooOld << '\n'
        << "  Thiếu " << packets.missing << "  |  Đến muộn đã bù " << packets.recovered
        << "  |  VALID:0 " << packets.invalidSensor << '\n'
        << "  Khoảng thiếu gần nhất ";
    if (packets.lastGap) {
        out << packets.lastGap->first << ".." << packets.lastGap->last;
    } else {
        out << "N/A";
    }
    out << "  |  Tỷ lệ nhận ước tính ";
    if (packets.unique == 0) {
        out << "N/A";
    } else {
        const auto received = static_cast<double>(packets.unique);
        const auto expected = received + static_cast<double>(packets.missing);
        out << std::fixed << std::setprecision(2) << 100.0 * received / expected << '%';
    }
    out << "\n  ACK đã gửi " << packets.acksSent << "  |  Lỗi gửi ACK " << packets.ackErrors
        << "  |  Phiên ESP32 " << packets.sensorBoots
        << "\n------------------------------------------------------------\nCtrl+C để dừng và lưu CSV.\n";
    return out.str();
}

template<class Service>
std::thread startWorker(Service& service, std::exception_ptr& error) {
    return std::thread([&service, &error] {
        try {
            service.run(stopRequested);
        } catch (...) {
            // Main chỉ đọc exception_ptr sau join, khi luồng này đã ghi xong.
            error = std::current_exception();
            stopRequested.store(true);
        }
    });
}

void reportError(const char* module, const std::exception_ptr& error) {
    if (!error) {
        return;
    }
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& ex) {
        std::cerr << module << ": " << ex.what() << '\n';
    } catch (...) {
        std::cerr << module << ": Loi khong xac dinh\n";
    }
}

int run(const Options& opts, const Terminal& terminal) {
    DataManager data(opts.sensorTimeout, opts.telemetryTimeout);
    Logger csv(opts.csv);
    UdpReceiver udp(data, opts.udpPort);
#if MONITOR_WITH_MAVSDK
    UavTelemetry uav(data, opts.mavsdkUrl, opts.telemetryTimeout);
#endif
    std::exception_ptr udpError, uavError, mainError;
    std::thread udpThread, uavThread;

    const auto writePending = [&] {
        for (const auto& row : data.drainCsv()) {
            csv.write(row);
        }
    };

    try {
        udpThread = startWorker(udp, udpError);
#if MONITOR_WITH_MAVSDK
        uavThread = startWorker(uav, uavError);
#endif
        auto nextRefresh = SteadyClock::now();
        while (!stopRequested.load()) {
            if (SteadyClock::now() >= nextRefresh) {
                writePending();
                const auto state = data.snapshot();
                const auto timestamp = WallClock::now();
                if (!state.sensor) {
                    // WAITING/STALE có hàng CSV riêng để thể hiện khoảng thiếu dữ liệu.
                    csv.write({timestamp, RecordType::Status, std::nullopt, state.sensorStatus, state.uav});
                }
                csv.flush();
                terminal.draw(dashboard(state, opts, timestamp));
                nextRefresh = SteadyClock::now() + opts.refresh;
            }
            std::this_thread::sleep_for(25ms);
        }
    } catch (...) {
        mainError = std::current_exception();
    }

    stopRequested.store(true);
    if (udpThread.joinable()) {
        udpThread.join();
    }
    if (uavThread.joinable()) {
        uavThread.join();
    }
    try {
        // Sau join không còn gói UDP mới. Ghi nốt các bản ghi chờ rồi flush.
        writePending();
        csv.flush();
    } catch (...) {
        if (!mainError) {
            mainError = std::current_exception();
        }
    }
    reportError("UDP", udpError);
    reportError("MAVSDK", uavError);
    reportError("Dashboard/CSV", mainError);
    if (udpError || uavError || mainError) {
        return 1;
    }
    // In bộ đếm sau join để số liệu đối soát bao gồm các gói ở chu kỳ cuối cùng.
    terminal.draw(dashboard(data.snapshot(), opts, WallClock::now()));
    std::cout << "Đã lưu CSV tại " << opts.csv.string() << '\n';
    return 0;
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        const auto opts = parseOptions(argc, argv);
        if (opts.help) {
            printHelp();
            return 0;
        }
        if (std::signal(SIGINT, onSigint) == SIG_ERR) {
            throw std::runtime_error("Khong dang ky duoc SIGINT");
        }
#ifdef _WIN32
        SetConsoleCtrlHandler(onConsoleControl, TRUE);
#endif
        const Terminal terminal(opts.plain);
        return run(opts, terminal);
    } catch (const std::exception& ex) {
        std::cerr << "esp32_px4_monitor: " << ex.what() << '\n';
        return 1;
    }
}
