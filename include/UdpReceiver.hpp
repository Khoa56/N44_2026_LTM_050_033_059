#pragma once

#include "DataManager.hpp"

#include <atomic>
#include <cstdint>

class UdpReceiver {
public:
    UdpReceiver(DataManager& data, std::uint16_t port);
    void run(const std::atomic<bool>& stop);

private:
    DataManager& data_;
    const std::uint16_t port_;
};
