#!/usr/bin/env bash
set -euo pipefail

runs=1
csv_path="test-son-bay.csv"
monitor_ip_arg=""

usage() {
    cat <<EOF
Cách dùng: bash $0 [--headless] [--runs N] [--csv FILE] [--ip IP]
Mở PX4 SITL + jMAVSim, bật luồng MAVLink UDP 14600 gửi tới bộ giám sát của Khoa,
rồi chạy kịch bản bay UAV mô phỏng (mặc định: 1 lượt, ghi vào test-son-bay.csv).
Biến môi trường PX4_MONITOR_IP (hoặc --ip): địa chỉ IPv4 máy Khoa;
đặt 'auto' để tự lấy IP Windows từ WSL2.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --headless)
            export HEADLESS=1
            shift
            ;;
        --runs)
            [[ $# -ge 2 ]] || { echo "Thiếu giá trị sau --runs." >&2; exit 2; }
            runs="$2"
            shift 2
            ;;
        --csv)
            [[ $# -ge 2 ]] || { echo "Thiếu đường dẫn sau --csv." >&2; exit 2; }
            csv_path="$2"
            shift 2
            ;;
        --ip)
            [[ $# -ge 2 ]] || { echo "Thiếu địa chỉ IP sau --ip." >&2; exit 2; }
            monitor_ip_arg="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Tham số không hợp lệ: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ ! "$runs" =~ ^[1-9][0-9]*$ ]]; then
    echo "--runs phải là số nguyên dương: $runs" >&2
    exit 2
fi

resolve_monitor_ip() {
    local raw="${monitor_ip_arg:-${PX4_MONITOR_IP:-auto}}"
    raw="${raw//$'\r'/}"
    raw="${raw//[[:space:]]/}"

    if [[ -z "$raw" || "$raw" == "auto" ]]; then
        if command -v wslinfo >/dev/null 2>&1 &&
           [[ "$(wslinfo --networking-mode 2>/dev/null || true)" == "mirrored" ]]; then
            printf '%s\n' "127.0.0.1"
            return 0
        fi
        local gateway=""
        gateway="$(ip route show default 2>/dev/null | awk '/^default/ {for (i=1; i<=NF; i++) if ($i=="via") {print $(i+1); exit}}' || true)"
        if [[ -n "$gateway" ]]; then
            printf '%s\n' "$gateway"
            return 0
        fi
        printf '%s\n' "127.0.0.1"
        return 0
    fi

    if [[ ! "$raw" =~ ^[A-Za-z0-9._-]+$ ]]; then
        echo "Địa chỉ PX4_MONITOR_IP không hợp lệ: $raw" >&2
        exit 2
    fi
    printf '%s\n' "$raw"
}

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
px4_dir="${PX4_DIR:-$HOME/PX4-Autopilot}"
px4_build_dir="$px4_dir/build/px4_sitl_default"
px4_bin_dir="$px4_build_dir/bin"
build_dir="${ESP32_PX4_BUILD_DIR:-build}"

if [[ ! -f "$px4_dir/Makefile" ]]; then
    echo "Chưa tìm thấy PX4 tại $px4_dir. Chạy uav.bat --setup trước." >&2
    exit 1
fi

java11=/usr/lib/jvm/java-11-openjdk-amd64
if [[ -z "${JAVA_HOME:-}" && -x "$java11/bin/java" ]]; then
    export JAVA_HOME="$java11"
fi
if [[ -n "${JAVA_HOME:-}" ]]; then
    export PATH="$JAVA_HOME/bin:$PATH"
fi

if [[ -z "${HEADLESS:-}" && -z "${DISPLAY:-}" ]]; then
    export HEADLESS=1
fi

export PX4_HOME_LAT="${PX4_HOME_LAT:-21.028511}"
export PX4_HOME_LON="${PX4_HOME_LON:-105.804817}"
export PX4_HOME_ALT="${PX4_HOME_ALT:-15.0}"

monitor_ip="$(resolve_monitor_ip)"

cd "$project_dir"
if [[ ! -f "$build_dir/CMakeCache.txt" || ! -x "$build_dir/uav_flight_scenario" ]]; then
    cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DMONITOR_WITH_MAVSDK=ON
fi
cmake --build "$build_dir" --target uav_flight_scenario --parallel

if [[ ! -x "$px4_bin_dir/px4" || ! -f "$px4_dir/Tools/simulation/jmavsim/jMAVSim/out/production/jmavsim_run.jar" ]]; then
    echo "Đang build PX4 SITL và jMAVSim (chỉ cần ở lần đầu)..."
    make -C "$px4_dir" px4_sitl
    make -C "$px4_dir" px4_sitl build_jmavsim
fi

if pgrep -x px4 >/dev/null 2>&1; then
    echo "PX4 SITL đang chạy ở một phiên khác. Hãy đóng phiên đó (gõ shutdown trong pxh>) rồi thử lại." >&2
    exit 1
fi

px4_pid=""
px4_started=0
px4_log="$(mktemp /tmp/px4-sitl-XXXXXX.log)"

cleanup() {
    local status=$?
    trap - EXIT INT TERM

    if [[ "$px4_started" -eq 1 ]]; then
        echo ""
        echo "[DỌN DẸP] Đang tắt PX4 SITL và jMAVSim..."
        if [[ -n "$px4_pid" ]] && kill -0 "$px4_pid" 2>/dev/null; then
            "$px4_bin_dir/px4-shutdown" >/dev/null 2>&1 || true
            for _ in $(seq 1 20); do
                kill -0 "$px4_pid" 2>/dev/null || break
                sleep 0.25
            done
            if kill -0 "$px4_pid" 2>/dev/null; then
                kill "$px4_pid" 2>/dev/null || true
                sleep 0.5
            fi
            if kill -0 "$px4_pid" 2>/dev/null; then
                kill -9 "$px4_pid" 2>/dev/null || true
            fi
            wait "$px4_pid" 2>/dev/null || true
        fi

        local jmavsim_pids=""
        jmavsim_pids="$(pgrep -f 'jmavsim_run\.jar' || true)"
        if [[ -n "$jmavsim_pids" ]]; then
            # shellcheck disable=SC2086
            kill $jmavsim_pids 2>/dev/null || true
        fi
    fi

    if [[ "$status" -ne 0 && -s "$px4_log" ]]; then
        echo "--- Nhật ký PX4 SITL gần nhất ($px4_log) ---" >&2
        tail -n 30 "$px4_log" >&2 || true
    else
        rm -f -- "$px4_log"
    fi
    exit "$status"
}
trap cleanup EXIT INT TERM

echo "======================================================================"
echo " KIỂM THỬ TÍCH HỢP KHỐI 2 (PX4 SITL) -> KHOA"
echo " - PX4 SITL      : $px4_dir (HEADLESS=${HEADLESS:-0})"
echo " - Máy Khoa      : $monitor_ip:14600 (MAVLink onboard)"
echo " - Kịch bản bay  : $runs lượt -> $csv_path"
echo "======================================================================"
echo "[1/3] Đang khởi động PX4 SITL + jMAVSim..."

PX4_SYS_AUTOSTART=10017 "$px4_bin_dir/px4" -d >"$px4_log" 2>&1 &
px4_pid=$!
px4_started=1

ready=0
for _ in $(seq 1 90); do
    if ! kill -0 "$px4_pid" 2>/dev/null; then
        echo "PX4 SITL đã thoát sớm khi khởi động." >&2
        exit 1
    fi
    if grep -q 'Startup script returned successfully' "$px4_log" 2>/dev/null; then
        ready=1
        break
    fi
    sleep 0.5
done

if [[ "$ready" -ne 1 ]]; then
    echo "Timeout (45s) chờ PX4 SITL khởi động xong." >&2
    exit 1
fi

echo "[2/3] Đang mở luồng MAVLink UDP 14600 tới $monitor_ip..."
"$px4_bin_dir/px4-mavlink" start -u 14601 -o 14600 -t "$monitor_ip" -m onboard -r 1000000
stream_ready=0
for _ in $(seq 1 20); do
    if grep -q 'remote port 14600' "$px4_log" 2>/dev/null; then
        stream_ready=1
        break
    fi
    sleep 0.25
done
if [[ "$stream_ready" -ne 1 ]]; then
    echo "Không thấy PX4 mở luồng MAVLink UDP 14600." >&2
    exit 1
fi
grep '\[mavlink\].*14600' "$px4_log" || true

echo "[3/3] Đang chạy kịch bản bay UAV ($runs lượt)..."
"$project_dir/$build_dir/uav_flight_scenario" --runs "$runs" --csv "$csv_path"
