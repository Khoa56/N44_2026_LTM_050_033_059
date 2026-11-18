#pragma once

#include "DataManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>

std::string utcTimestamp(WallClock::time_point timestamp);

class Logger {
public:
    explicit Logger(const std::filesystem::path& path);
    void write(const CsvRecord& record);
    void flush();

private:
    std::ofstream stream_;
};
