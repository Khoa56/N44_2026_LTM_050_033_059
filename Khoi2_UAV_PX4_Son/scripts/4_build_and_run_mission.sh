#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$project_dir"
build_dir="${ESP32_PX4_BUILD_DIR:-build}"
cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DMONITOR_WITH_MAVSDK=ON
cmake --build "$build_dir" --parallel
exec "$(cd "$build_dir" && pwd)/uav_flight_scenario" "$@"
