cmake_minimum_required(VERSION 3.22)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(uav_px4_scenario LANGUAGES CXX)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    find_package(MAVSDK 3.17...<4 CONFIG REQUIRED)
endif()

add_executable(uav_flight_scenario src/uav_flight_scenario.cpp)
set_target_properties(uav_flight_scenario PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_link_libraries(uav_flight_scenario PRIVATE MAVSDK::mavsdk Threads::Threads)

if(MSVC)
    target_compile_options(uav_flight_scenario PRIVATE /W4 /utf-8)
else()
    target_compile_options(uav_flight_scenario PRIVATE -Wall -Wextra -Wpedantic)
endif()
