#include "Logger.hpp"

#include <ctime>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {
constexpr std::string_view header =
    "Timestamp_UTC,Record_Type,Seq_Num,Temperature_C,Humidity_pctRH,Sensor_Valid,Sensor_Status,"
    "UAV_Connected,UAV_Link,Flight_Mode,In_Air,Latitude_deg,Longitude_deg,Altitude_m,UAV_Telemetry_Stale,"
    "Sensor_Session,Sensor_Uptime_ms";

template<class T>
void writeValue(std::ostream& out, const std::optional<T>& value) {
    if (value) {
        out << *value;
    } else {
        out << "N/A";
    }
}

void writeCsvText(std::ostream& out, const std::optional<std::string>& value) {
    if (!value || value->find_first_of(",\"\r\n") == std::string::npos) {
        writeValue(out, value);
        return;
    }
    out << '"';
    for (const char ch : *value) {
        if (ch == '"') {
            out << '"';
        }
        out << ch;
    }
    out << '"';
}
} // namespace

std::string utcTimestamp(WallClock::time_point timestamp) {
    using namespace std::chrono;
    const auto wholeSeconds = floor<seconds>(timestamp);
    const auto fraction = duration_cast<milliseconds>(timestamp - wholeSeconds).count();
    const auto epoch = WallClock::to_time_t(wholeSeconds);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &epoch) != 0) {
#else
    if (gmtime_r(&epoch, &utc) == nullptr) {
#endif
        throw std::runtime_error("Khong doi duoc timestamp sang UTC");
    }
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
         << std::setw(3) << std::setfill('0') << fraction << 'Z';
    return text.str();
}

Logger::Logger(const std::filesystem::path& path) {
    // Ghi nối tiếp an toàn cho các phiên chạy. Kiểm tra header và kết thúc dòng trước khi nối.
    bool writeHeader = true;
    if (std::filesystem::exists(path) && std::filesystem::file_size(path) != 0) {
        std::ifstream existing(path, std::ios::binary);
        std::string firstLine;
        std::getline(existing, firstLine);
        if (!firstLine.empty() && firstLine.back() == '\r') {
            firstLine.pop_back();
        }
        existing.seekg(-1, std::ios::end);
        if (firstLine != header || existing.get() != '\n') {
            throw std::runtime_error("CSV da co header khac hoac dong cuoi bi cat. Chon tep moi.");
        }
        writeHeader = false;
    }

    stream_.exceptions(std::ios::failbit | std::ios::badbit);
    stream_.open(path, std::ios::binary | std::ios::app);
    stream_.imbue(std::locale::classic());
    stream_ << std::setprecision(12) << std::boolalpha;
    if (writeHeader) {
        stream_ << header << '\n';
    }
    flush();
}

void Logger::write(const CsvRecord& record) {
    stream_ << utcTimestamp(record.timestamp) << ',' << label(record.type) << ',';
    if (record.sensor) {
        stream_ << record.sensor->seq << ',';
        writeValue(stream_, record.sensor->temperatureC);
        stream_ << ',';
        writeValue(stream_, record.sensor->humidityPct);
        stream_ << ',' << (record.sensor->valid ? '1' : '0');
    } else {
        stream_ << "N/A,N/A,N/A,N/A";
    }
    const auto& uav = record.uav;
    stream_ << ',' << label(record.sensorStatus) << ',' << (uav.link == UavLink::Connected)
            << ',' << label(uav.link) << ',';
    writeCsvText(stream_, uav.flightMode);
    stream_ << ',';
    writeValue(stream_, uav.inAir);
    stream_ << ',';
    writeValue(stream_, uav.position.latitudeDeg);
    stream_ << ',';
    writeValue(stream_, uav.position.longitudeDeg);
    stream_ << ',';
    writeValue(stream_, uav.position.relativeAltitudeM);
    stream_ << ',';
    if (uav.link != UavLink::Waiting) {
        stream_ << uav.telemetryStale;
    } else {
        stream_ << "N/A";
    }
    stream_ << ',';
    if (record.sensor) {
        writeValue(stream_, record.sensor->session);
        stream_ << ',';
        writeValue(stream_, record.sensor->uptimeMs);
    } else {
        stream_ << "N/A,N/A";
    }
    stream_ << '\n';
}

void Logger::flush() {
    stream_.flush();
}
