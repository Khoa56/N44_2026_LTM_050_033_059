#!/usr/bin/env bash
set -euo pipefail

case "${1:-}" in
    "") ;;
    --headless) export HEADLESS=1 ;;
    -h|--help) echo "Cách dùng: bash $0 [--headless]"; exit 0 ;;
    *) echo "Tham số không hợp lệ: $1" >&2; exit 2 ;;
esac
if [[ $# -gt 1 ]]; then
    echo "Cách dùng: bash $0 [--headless]" >&2
    exit 2
fi

px4_dir="${PX4_DIR:-$HOME/PX4-Autopilot}"
if [[ ! -f "$px4_dir/Makefile" ]]; then
    echo "Chưa tìm thấy PX4 tại $px4_dir. Cài môi trường theo README." >&2
    exit 1
fi

export PX4_HOME_LAT="${PX4_HOME_LAT:-21.028511}"
export PX4_HOME_LON="${PX4_HOME_LON:-105.804817}"
export PX4_HOME_ALT="${PX4_HOME_ALT:-15.0}"
java11=/usr/lib/jvm/java-11-openjdk-amd64
if [[ -z "${JAVA_HOME:-}" && -x "$java11/bin/java" ]]; then
    export JAVA_HOME="$java11"
fi
if [[ -n "${JAVA_HOME:-}" ]]; then
    export PATH="$JAVA_HOME/bin:$PATH"
fi

echo "Khi thấy pxh>, mở luồng MAVLink cho bộ giám sát cùng máy bằng lệnh:"
echo "mavlink start -u 14601 -o 14600 -t 127.0.0.1 -m onboard -r 1000000"
cd "$px4_dir"
exec make px4_sitl jmavsim
