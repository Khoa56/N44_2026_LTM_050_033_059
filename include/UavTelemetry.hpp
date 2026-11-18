#pragma once

#include "DataManager.hpp"

#include <atomic>
#include <chrono>
#include <string>

class UavTelemetry {
public:
    UavTelemetry(DataManager& data, std::string url, std::chrono::milliseconds timeout);
    void run(const std::atomic<bool>& stop);

private:
    DataManager& data_;
    const std::string url_;
    const std::chrono::milliseconds timeout_;
};
