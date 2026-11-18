/**
 * @file uav_flight_scenario.cpp
 * @brief Chương trình điều khiển kịch bản bay tự động UAV mô phỏng PX4 bằng MAVSDK C++
 *        Phụ trách: Sơn (Khối 2 - UAV Mô phỏng PX4)
 *
 * Chức năng chính:
 *  1. Kết nối tới PX4 SITL qua UDP 14540. Bộ giám sát dùng liên kết riêng trên UDP 14600.
 *  2. Giám sát và hiển thị 5 thông số trạng thái UAV:
 *     - Trạng thái kết nối (Connected / Disconnected)
 *     - Chế độ bay (Flight Mode)
 *     - Trạng thái đang bay (In-Air / On-Ground & Armed / Disarmed)
 *     - Vị trí GPS (Latitude, Longitude)
 *     - Độ cao tương đối & tuyệt đối (Relative / Absolute Altitude - đơn vị: m)
 *  3. Kiểm tra an toàn trước khi bay (Pre-flight Health Checks: GPS, IMU, Home, Armable).
 *  4. Thực thi kịch bản bay tự động:
 *     Cất cánh (Takeoff) -> Bay qua 4 điểm định trước (Waypoints) -> Quay về hạ cánh (RTL / Land).
 *  5. Xử lý lỗi & Timeout an toàn; hỗ trợ chạy lặp nhiều lượt bay (--runs N) và ghi nhật ký CSV.
 */

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/action/action.h>
#include <mavsdk/plugins/mission/mission.h>
#include <mavsdk/plugins/telemetry/telemetry.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace mavsdk;
using std::chrono::seconds;
using std::chrono::steady_clock;
using std::this_thread::sleep_for;

namespace {

// Lấy chuỗi thời gian hiện tại phục vụ in log và lưu nhật ký kiểm thử
std::string current_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &now_c);
#else
    gmtime_r(&now_c, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

// Tạo một điểm bay (MissionItem) trong kịch bản
Mission::MissionItem make_mission_item(
    double latitude_deg,
    double longitude_deg,
    float relative_altitude_m,
    float speed_m_s,
    bool is_fly_through) {
    Mission::MissionItem item{};
    item.latitude_deg = latitude_deg;
    item.longitude_deg = longitude_deg;
    item.relative_altitude_m = relative_altitude_m;
    item.speed_m_s = speed_m_s;
    item.is_fly_through = is_fly_through;
    item.gimbal_pitch_deg = std::numeric_limits<float>::quiet_NaN();
    item.gimbal_yaw_deg = std::numeric_limits<float>::quiet_NaN();
    item.camera_action = Mission::MissionItem::CameraAction::None;
    item.loiter_time_s = std::numeric_limits<float>::quiet_NaN();
    item.camera_photo_interval_s = 1.0f;
    item.acceptance_radius_m = 2.0f;
    item.yaw_deg = std::numeric_limits<float>::quiet_NaN();
    item.camera_photo_distance_m = std::numeric_limits<float>::quiet_NaN();
    return item;
}

// In tóm tắt trạng thái 5 thông số UAV ra màn hình
void print_uav_status_snapshot(
    const std::shared_ptr<System>& system,
    Telemetry& telemetry,
    const std::string& phase_label) {
    const bool connected = system->is_connected();
    const auto flight_mode = telemetry.flight_mode();
    const bool in_air = telemetry.in_air();
    const bool armed = telemetry.armed();
    const auto pos = telemetry.position();

    std::cout << "[" << current_timestamp() << "] [TRẠNG THÁI UAV - " << phase_label << "]\n"
              << "  + Kết nối     : " << (connected ? "CONNECTED (Đã kết nối)" : "DISCONNECTED (Mất kết nối)") << "\n"
              << "  + Chế độ bay  : " << flight_mode << "\n"
              << "  + Đang bay    : " << (in_air ? "IN_AIR (Đang trên không)" : "ON_GROUND (Trên mặt đất)")
              << " | Động cơ: " << (armed ? "ARMED" : "DISARMED") << "\n"
              << std::fixed << std::setprecision(6)
              << "  + Vị trí GPS  : Lat = " << pos.latitude_deg << " deg, Lon = " << pos.longitude_deg << " deg\n"
              << std::setprecision(2)
              << "  + Độ cao      : " << pos.relative_altitude_m << " m (tương đối) | "
              << pos.absolute_altitude_m << " m (tuyệt đối AMSL)\n";
}

// Thực hiện 1 lượt kịch bản bay hoàn chỉnh và trả về true nếu thành công
bool execute_single_flight_run(
    int run_index,
    int total_runs,
    const std::shared_ptr<System>& system,
    Telemetry& telemetry,
    Action& action,
    Mission& mission,
    float& out_max_altitude_m,
    int& out_waypoints_completed,
    std::string& out_note) {

    out_max_altitude_m = 0.0f;
    out_waypoints_completed = 0;

    std::cout << "\n======================================================================\n"
              << " BẮT ĐẦU LƯỢT BAY " << run_index << " / " << total_runs
              << " (" << current_timestamp() << ")\n"
              << "======================================================================\n";

    // -------------------------------------------------------------------------
    // Giai đoạn 1: Kiểm tra điều kiện sẵn sàng trước khi bay (Pre-flight Checks)
    // -------------------------------------------------------------------------
    std::cout << "[Bước 1/5] Kiểm tra điều kiện an toàn trước khi bay (Pre-flight Health Checks)...\n";
    const auto health_start = steady_clock::now();
    bool switched_to_hold = false;
    for (;;) {
        if (!system->is_connected()) {
            out_note = "Mất kết nối UAV trong khi kiểm tra Pre-flight";
            std::cerr << "[LỖI] " << out_note << "\n";
            return false;
        }
        const auto landed = telemetry.landed_state();
        if (telemetry.armed() ||
            (landed != Telemetry::LandedState::Unknown && landed != Telemetry::LandedState::OnGround)) {
            out_note = "Cần UAV ở mặt đất và đã tắt động cơ";
            return false;
        }
        const auto mode = telemetry.flight_mode();
        if (!switched_to_hold && landed == Telemetry::LandedState::OnGround &&
            (mode == Telemetry::FlightMode::Land || mode == Telemetry::FlightMode::ReturnToLaunch)) {
            // PX4 có thể giữ chế độ hạ cánh của lượt trước và từ chối arm.
            const auto result = action.hold();
            if (result != Action::Result::Success) {
                out_note = "Không chuyển được sang Hold sau khi hạ cánh";
                std::cerr << "[LỖI] Hold: " << result << '\n';
                return false;
            }
            switched_to_hold = true;
            sleep_for(seconds(1));
            continue;
        }
        const auto h = telemetry.health();
        if (landed == Telemetry::LandedState::OnGround && telemetry.health_all_ok() && h.is_armable) {
            break;
        }
        std::cout << "  -> Đang đợi cảm biến & GPS sẵn sàng: "
                  << "Gyro=" << h.is_gyrometer_calibration_ok << " "
                  << "Accel=" << h.is_accelerometer_calibration_ok << " "
                  << "Mag=" << h.is_magnetometer_calibration_ok << " "
                  << "LocalPos=" << h.is_local_position_ok << " "
                  << "GlobalPos=" << h.is_global_position_ok << " "
                  << "HomePos=" << h.is_home_position_ok << " "
                  << "Armable=" << h.is_armable << "\n";

        if (steady_clock::now() - health_start > seconds(45)) {
            out_note = "Timeout (45s) khi chờ điều kiện Pre-flight Health OK";
            std::cerr << "[LỖI] " << out_note << "\n";
            return false;
        }
        sleep_for(seconds(2));
    }
    std::cout << "[OK] Tất cả điều kiện Pre-flight (GPS, IMU, Home, Armable) đã sẵn sàng!\n";
    print_uav_status_snapshot(system, telemetry, "TRƯỚC KHI CẤT CÁNH");

    // -------------------------------------------------------------------------
    // Giai đoạn 2: Xây dựng & Tải kịch bản bay (4 điểm Waypoints quanh vị trí Home)
    // -------------------------------------------------------------------------
    const auto home_pos = telemetry.home();
    const double base_lat = home_pos.latitude_deg;
    const double base_lon = home_pos.longitude_deg;
    if (!std::isfinite(base_lat) || !std::isfinite(base_lon)) {
        out_note = "Tọa độ Home chưa có";
        return false;
    }
    const float cruise_alt_m = 10.0f;
    const float cruise_speed_m_s = 5.0f;

    // Độ lệch ~0.0004 độ (~44 mét) tạo thành lộ trình hình vuông rõ ràng quanh điểm xuất phát
    const double offset_deg = 0.0004;

    std::cout << "[Bước 2/5] Khởi tạo và nạp kịch bản 4 điểm bay (Waypoints) ở độ cao "
              << cruise_alt_m << " m...\n";

    std::vector<Mission::MissionItem> mission_items;
    // Điểm 1: Bay về hướng Bắc (~44m)
    mission_items.push_back(make_mission_item(
        base_lat + offset_deg, base_lon, cruise_alt_m, cruise_speed_m_s, true));
    // Điểm 2: Bay về hướng Đông Bắc (~44m Đông)
    mission_items.push_back(make_mission_item(
        base_lat + offset_deg, base_lon + offset_deg, cruise_alt_m, cruise_speed_m_s, true));
    // Điểm 3: Bay về hướng Đông (~44m Nam)
    mission_items.push_back(make_mission_item(
        base_lat, base_lon + offset_deg, cruise_alt_m, cruise_speed_m_s, true));
    // Điểm 4: Quay về phía trên vị trí xuất phát
    mission_items.push_back(make_mission_item(
        base_lat, base_lon, cruise_alt_m, cruise_speed_m_s, false));

    Mission::MissionPlan mission_plan{};
    mission_plan.mission_items = mission_items;

    const Mission::Result upload_result = mission.upload_mission(mission_plan);
    if (upload_result != Mission::Result::Success) {
        out_note = "Tải kịch bản bay thất bại: " + std::to_string(static_cast<int>(upload_result));
        std::cerr << "[LỖI] Không thể nạp kịch bản bay lên PX4: " << upload_result << "\n";
        return false;
    }
    std::cout << "[OK] Đã nạp thành công 4 điểm bay lên UAV.\n";

    // Theo dõi tiến độ từng điểm bay và độ cao lớn nhất đạt được
    const int total_wp = static_cast<int>(mission_items.size());

    // -------------------------------------------------------------------------
    // Giai đoạn 3: Arm động cơ & Bắt đầu kịch bản bay (Cất cánh + Bay qua các điểm)
    // -------------------------------------------------------------------------
    std::cout << "[Bước 3/5] Khởi động động cơ (Arming) và bắt đầu kịch bản bay tự động...\n";
    const Action::Result arm_result = action.arm();
    if (arm_result != Action::Result::Success) {
        out_note = "Lệnh Arm thất bại";
        std::cerr << "[LỖI] Không thể Arm động cơ: " << arm_result << "\n";
        return false;
    }
    std::cout << "[OK] Đã Arm động cơ thành công.\n";

    const Mission::Result start_result = mission.start_mission();
    if (start_result != Mission::Result::Success) {
        std::cerr << "[LỖI] Khởi chạy kịch bản thất bại: " << start_result << " -> Kích hoạt Hạ cánh an toàn!\n";
        action.land();
        out_note = "Khởi chạy Mission thất bại";
        return false;
    }

    // Vòng lặp giám sát quá trình bay qua các điểm với cơ chế Timeout (tối đa 120 giây)
    const auto mission_start = steady_clock::now();
    bool mission_completed = false;

    while (steady_clock::now() - mission_start < seconds(120)) {
        if (!system->is_connected()) {
            out_note = "Mất kết nối với PX4 khi đang bay";
            std::cerr << "[CẢNH BÁO] " << out_note << "\n";
            return false;
        }

        const float current_alt = telemetry.position().relative_altitude_m;
        out_waypoints_completed = std::clamp(mission.mission_progress().current, 0, total_wp);
        if (current_alt > out_max_altitude_m) {
            out_max_altitude_m = current_alt;
        }

        print_uav_status_snapshot(
            system,
            telemetry,
            "ĐANG BAY WAYPOINT " + std::to_string(out_waypoints_completed) + "/" + std::to_string(total_wp));

        auto [res, finished] = mission.is_mission_finished();
        if (res == Mission::Result::Success && finished) {
            mission_completed = true;
            out_waypoints_completed = total_wp;
            break;
        }

        sleep_for(seconds(2));
    }

    if (!mission_completed) {
        out_note = "Timeout (120s) khi bay qua các Waypoints -> Tự động kích hoạt RTL an toàn";
        std::cerr << "[CẢNH BÁO] " << out_note << "\n";
        if (action.return_to_launch() != Action::Result::Success) {
            action.land();
        }
        return false;
    }

    std::cout << "[OK] UAV đã hoàn thành toàn bộ " << total_wp << " điểm bay định trước!\n";

    // -------------------------------------------------------------------------
    // Giai đoạn 4: Quay về điểm xuất phát và Hạ cánh (Return-to-Launch / Land)
    // -------------------------------------------------------------------------
    std::cout << "[Bước 4/5] Ra lệnh Quay về điểm xuất phát và Hạ cánh (Return-to-Launch - RTL)...\n";
    const Action::Result rtl_result = action.return_to_launch();
    if (rtl_result != Action::Result::Success) {
        std::cerr << "[CẢNH BÁO] Lệnh RTL trả về " << rtl_result << " -> Chuyển sang lệnh Land trực tiếp...\n";
        const auto land_result = action.land();
        if (land_result != Action::Result::Success) {
            out_note = "Cả hai lệnh RTL và Land đều thất bại";
            return false;
        }
    }

    // -------------------------------------------------------------------------
    // Giai đoạn 5: Đợi UAV chạm đất (in_air == false) và tắt động cơ (Disarmed)
    // -------------------------------------------------------------------------
    std::cout << "[Bước 5/5] Đang theo dõi quá trình hạ cánh cho đến khi chạm đất và tắt động cơ...\n";
    const auto land_start = steady_clock::now();
    for (;;) {
        if (!system->is_connected()) {
            out_note = "Mất kết nối trong khi đang hạ cánh";
            return false;
        }
        if (telemetry.landed_state() == Telemetry::LandedState::OnGround && !telemetry.armed()) {
            break;
        }
        print_uav_status_snapshot(system, telemetry, "ĐANG HẠ CÁNH (RTL/LAND)");
        if (steady_clock::now() - land_start > seconds(90)) {
            out_note = "Timeout (90s) khi chờ hạ cánh và tắt động cơ";
            std::cerr << "[LỖI] " << out_note << "\n";
            return false;
        }
        sleep_for(seconds(2));
    }

    print_uav_status_snapshot(system, telemetry, "ĐÃ HẠ CÁNH & HOÀN TẤT");
    out_note = "Hoàn thành kịch bản bay an toàn";
    std::cout << "[THÀNH CÔNG] Lượt bay " << run_index << "/" << total_runs << " đã hoàn tất 100%!\n";
    return true;
}

} // namespace

int main(int argc, char** argv) try {
    std::string connection_url = "udpin://127.0.0.1:14540";
    std::filesystem::path csv_path = "flight-test-v3.csv";
    int total_runs = 1;
    bool has_url = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--runs") {
            if (++i == argc) {
                throw std::runtime_error("Thiếu số lượt sau --runs");
            }
            const std::string_view value = argv[i];
            const auto end = value.data() + value.size();
            const auto parsed = std::from_chars(value.data(), end, total_runs);
            if (parsed.ec != std::errc{} || parsed.ptr != end || total_runs <= 0) {
                throw std::runtime_error("--runs phải là số nguyên dương");
            }
        } else if (arg == "--csv") {
            if (++i == argc || std::string_view(argv[i]).empty() ||
                std::string_view(argv[i]).substr(0, 2) == "--") {
                throw std::runtime_error("Thiếu đường dẫn sau --csv");
            }
            csv_path = argv[i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Cách dùng: " << argv[0] << " [udpin://IP:PORT] [--runs N] [--csv FILE]\n"
                      << "  Cổng điều khiển mặc định: udpin://127.0.0.1:14540 (PX4 SITL cùng máy)\n"
                      << "  Nhật ký mặc định: flight-test-v3.csv\n";
            return 0;
        } else if (!has_url && arg.substr(0, 8) == "udpin://") {
            connection_url = arg;
            has_url = true;
        } else if (!has_url && arg.substr(0, 7) == "udp://:") {
            connection_url = "udpin://0.0.0.0:" + std::string(arg.substr(7));
            has_url = true;
        } else {
            throw std::runtime_error("Tham số không hợp lệ: " + std::string(arg));
        }
    }

    std::cout << "======================================================================\n"
              << " CHƯƠNG TRÌNH ĐIỀU KHIỂN KỊCH BẢN BAY UAV MÔ PHỎNG PX4 (MAVSDK C++)\n"
              << " - Cổng điều khiển (Sơn) : " << connection_url << "\n"
              << " - Cổng giám sát (Khoa)  : UDP 14600 (luồng MAVLink riêng)\n"
              << " - Số lượt bay thực hiện : " << total_runs << "\n"
              << "======================================================================\n";

    Mavsdk mavsdk{Mavsdk::Configuration{ComponentType::GroundStation}};
    const ConnectionResult conn_result = mavsdk.add_any_connection(connection_url);
    if (conn_result != ConnectionResult::Success) {
        std::cerr << "[LỖI KẾT NỐI] Không thể mở cổng " << connection_url << ": " << conn_result << "\n";
        return 1;
    }

    std::cout << "[ĐANG CHỜ] Đang tìm kiếm UAV mô phỏng PX4 trên " << connection_url << " (Timeout 15s)...\n";
    auto system_opt = mavsdk.first_autopilot(15.0);
    if (!system_opt) {
        std::cerr << "[LỖI TIMEOUT] Không tìm thấy UAV mô phỏng PX4 sau 15 giây!\n"
                  << " -> Chạy PX4 SITL theo README ở thư mục dự án.\n";
        return 1;
    }

    auto system = system_opt.value();
    std::cout << "[ĐÃ KẾT NỐI] Đã phát hiện UAV Autopilot (System ID: " << static_cast<int>(system->get_system_id()) << ")\n";

    // Khởi tạo các plugin MAVSDK
    Telemetry telemetry{system};
    Action action{system};
    Mission mission{system};

    // Thiết lập tần suất cập nhật dữ liệu trạng thái UAV
    telemetry.set_rate_position(2.0);
    telemetry.set_rate_in_air(2.0);

    // Đăng ký lắng nghe sự kiện thay đổi kết nối & chế độ bay theo thời gian thực
    system->subscribe_is_connected([](bool is_connected) {
        std::cout << "[" << current_timestamp() << "] [SỰ KIỆN KẾT NỐI] UAV "
                  << (is_connected ? "ĐÃ KẾT NỐI (CONNECTED)" : "MẤT KẾT NỐI (DISCONNECTED)!") << "\n";
    });

    telemetry.subscribe_flight_mode([](Telemetry::FlightMode flight_mode) {
        std::cout << "[" << current_timestamp() << "] [CHUYỂN CHẾ ĐỘ BAY] FlightMode -> " << flight_mode << "\n";
    });

    constexpr std::string_view header =
        "run_index,start_time,end_time,duration_s,max_altitude_m,waypoints_completed,result,note";
    const bool has_rows = std::filesystem::exists(csv_path) && std::filesystem::file_size(csv_path) > 0;
    if (has_rows) {
        std::ifstream existing(csv_path, std::ios::binary);
        std::string first_line;
        std::getline(existing, first_line);
        if (!first_line.empty() && first_line.back() == '\r') {
            first_line.pop_back();
        }
        existing.seekg(-1, std::ios::end);
        if (first_line != header || existing.get() != '\n') {
            throw std::runtime_error("CSV lượt bay có header khác hoặc dòng cuối bị cắt. Chọn tệp mới.");
        }
    }
    std::ofstream csv_file;
    csv_file.exceptions(std::ios::failbit | std::ios::badbit);
    csv_file.open(csv_path, std::ios::app);
    csv_file.imbue(std::locale::classic());
    if (!has_rows) {
        csv_file << header << '\n' << std::flush;
    }

    int success_count = 0;

    for (int index = 0; index < total_runs; ++index) {
        const int run = index + 1;
        const std::string start_ts = current_timestamp();
        const auto run_start_clock = steady_clock::now();
        float max_alt_m = 0.0f;
        int wp_done = 0;
        std::string note;

        bool ok = execute_single_flight_run(
            run, total_runs, system, telemetry, action, mission, max_alt_m, wp_done, note);

        const auto duration_s = std::chrono::duration_cast<seconds>(steady_clock::now() - run_start_clock).count();
        const std::string end_ts = current_timestamp();

        if (ok) {
            ++success_count;
        }

        csv_file << run << "," << start_ts << "," << end_ts << "," << duration_s << ","
                 << std::fixed << std::setprecision(2) << max_alt_m << "," << wp_done << "/4,"
                 << (ok ? "PASS" : "FAIL") << "," << "\"" << note << "\"\n" << std::flush;
        if (!ok) {
            std::cerr << "[LỖI] " << note << ". Dừng các lượt bay tiếp theo.\n";
            break;
        }

        if (run < total_runs) {
            std::cout << "\n[NGHỈ GIỮA LƯỢT] Đợi 5 giây trước khi bắt đầu lượt bay tiếp theo...\n";
            sleep_for(seconds(5));
        }
    }

    std::cout << "\n======================================================================\n"
              << " TỔNG KẾT KIỂM THỬ KỊCH BẢN BAY UAV (KHỐI 2 - SƠN)\n"
              << " - Số lượt hoàn thành : " << success_count << " / " << total_runs
              << " (" << std::fixed << std::setprecision(1)
              << (100.0 * success_count / total_runs) << "%)\n"
              << " - Nhật ký lưu tại    : " << csv_path.string() << "\n"
              << "======================================================================\n";

    return (success_count == total_runs) ? 0 : 2;
} catch (const std::exception& error) {
    std::cerr << "uav_flight_scenario: " << error.what() << '\n';
    return 1;
}
