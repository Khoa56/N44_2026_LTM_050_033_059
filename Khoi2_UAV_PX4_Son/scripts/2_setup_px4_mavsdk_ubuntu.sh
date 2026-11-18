#!/usr/bin/env bash
set -euo pipefail

. /etc/os-release
if [[ "$ID" != ubuntu || "$VERSION_ID" != 22.04 || "$(uname -m)" != x86_64 ]]; then
    echo "Script cài PX4 v1.14.3 và jMAVSim cho Ubuntu 22.04 x86-64." >&2
    exit 1
fi

px4_dir="${PX4_DIR:-$HOME/PX4-Autopilot}"
px4_version=v1.14.3
mavsdk_version=3.17.5

sudo apt-get update
sudo apt-get install -y build-essential cmake curl git ninja-build python3-pip \
    openjdk-11-jdk ant bc libvecmath-java libgl1-mesa-dri libglx-mesa0 libglu1-mesa

if [[ ! -e "$px4_dir" ]]; then
    git clone --branch "$px4_version" --recurse-submodules \
        https://github.com/PX4/PX4-Autopilot.git "$px4_dir"
elif [[ "$(git -C "$px4_dir" describe --tags --exact-match HEAD 2>/dev/null || true)" != "$px4_version" ]]; then
    echo "Cần bản PX4 $px4_version. Đặt PX4_DIR tới một thư mục riêng." >&2
    exit 1
fi
git -C "$px4_dir" submodule update --init --recursive
bash "$px4_dir/Tools/setup/ubuntu.sh" --no-nuttx --no-sim-tools
python3 -m pip install --user 'empy==3.3.4'

installed="$(dpkg-query -W -f='${Version}' libmavsdk-dev 2>/dev/null || true)"
if [[ "$installed" != "$mavsdk_version" ]]; then
    sdk_pkg="libmavsdk-dev_${mavsdk_version}_ubuntu22.04_amd64.deb"
    temp_dir="$(mktemp -d)"
    trap 'rm -f -- "$temp_dir/$sdk_pkg"; rmdir -- "$temp_dir"' EXIT
    curl -fL --retry 2 -o "$temp_dir/$sdk_pkg" \
        "https://github.com/mavlink/MAVSDK/releases/download/v${mavsdk_version}/$sdk_pkg"
    sudo apt-get install -y "$temp_dir/$sdk_pkg"
fi

printf 'PX4 %s tại %s, MAVSDK %s. Tiếp tục theo README ở thư mục dự án.\n' \
    "$px4_version" "$px4_dir" "$mavsdk_version"
