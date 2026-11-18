cmake_minimum_required(VERSION 3.22)
project(esp32_px4 VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MONITOR_WITH_MAVSDK "Build the PX4 telemetry receiver using MAVSDK v3" ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(esp32_px4_monitor
    src/main.cpp
    src/UdpReceiver.cpp
    src/DataManager.cpp
    src/Logger.cpp
)
target_include_directories(esp32_px4_monitor PRIVATE include)
target_include_directories(esp32_px4_monitor SYSTEM PRIVATE
    ESP32-C3-LTMProject-Tuan/include
    ESP32-C3-LTMProject-Tuan/pc/third_party
)
target_link_libraries(esp32_px4_monitor PRIVATE Threads::Threads)

if(MONITOR_WITH_MAVSDK)
    # MAVSDK 3.17 cung cấp API C++17 và target gồm plugin Telemetry.
    find_package(MAVSDK 3.17...<4 CONFIG REQUIRED)
    target_sources(esp32_px4_monitor PRIVATE src/UavTelemetry.cpp)
    target_link_libraries(esp32_px4_monitor PRIVATE MAVSDK::mavsdk)
    add_subdirectory(Khoi2_UAV_PX4_Son)
endif()
target_compile_definitions(esp32_px4_monitor PRIVATE MONITOR_WITH_MAVSDK=$<BOOL:${MONITOR_WITH_MAVSDK}>)

if(WIN32)
    target_link_libraries(esp32_px4_monitor PRIVATE ws2_32)
    target_compile_definitions(esp32_px4_monitor PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(MSVC)
    target_compile_options(esp32_px4_monitor PRIVATE /W4 /utf-8)
else()
    target_compile_options(esp32_px4_monitor PRIVATE -Wall -Wextra -Wpedantic)
endif()
