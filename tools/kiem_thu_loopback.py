#!/usr/bin/env python3
"""Đo đường UDP loopback với tiến trình giám sát C++ đang chạy thật."""

import argparse
import csv
import datetime as dt
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import time

from mock_esp32 import events_for, payload_for


COUNTERS = re.compile(
    r"UDP +Tổng (\d+) +\| +Đúng định dạng, duy nhất (\d+)\n"
    r" +Sai định dạng (\d+) +\| +Trùng (\d+) +\| +Quá cửa sổ (\d+)\n"
    r" +Thiếu (\d+) +\| +Đến muộn đã bù (\d+) +\| +VALID:0 (\d+)"
)
FIELDS = ("received", "unique", "malformed", "duplicate", "too_old", "missing_seq", "late", "invalid_sensor")


def stop_monitor(process):
    # Ctrl+Break nhắm đúng nhóm tiến trình trên Windows và đi qua cùng hàm tắt với Ctrl+C.
    interrupt = signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT
    process.send_signal(interrupt)
    if process.wait(timeout=8) != 0:
        raise RuntimeError("Chương trình C++ trả mã lỗi khi dừng. Xem nhật ký console.")


def measure(binary, port, destination, case, transmissions, expected, expected_seq, expected_acks=None):
    csv_path = destination / f"{case}.csv"
    log_path = destination / f"{case}.txt"
    if csv_path.exists() or log_path.exists():
        raise FileExistsError(f"Dùng thư mục kết quả mới. Đã có dữ liệu cho {case}.")
    command = [str(binary), "--udp-port", str(port), "--timeout-ms", "3000",
               "--refresh-ms", "500", "--csv", str(csv_path), "--plain"]
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    count_sent = 0
    with log_path.open("wb") as output, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT, creationflags=flags)
        try:
            # Cho tiến trình nhận hoàn tất khởi tạo cổng trước khi đo.
            deadline = time.monotonic() + 5
            while not csv_path.exists() and process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.05)
            time.sleep(0.35)
            if process.poll() is not None or not csv_path.exists():
                raise RuntimeError(f"Bộ nhận chưa sẵn sàng. Xem {log_path.name}.")

            endpoint = ("127.0.0.1", port)
            source.settimeout(0.5)
            last_sent = time.monotonic()
            acks = []
            for index, (delay, datagram) in enumerate(transmissions):
                time.sleep(max(0.0, last_sent + delay - time.monotonic()))
                source.sendto(datagram, endpoint)
                count_sent += 1
                last_sent = time.monotonic()
                if expected_acks is not None:
                    try:
                        reply, peer = source.recvfrom(256)
                        if peer != endpoint:
                            raise AssertionError(f"ACK đến từ sai địa chỉ/cổng: {peer}")
                    except socket.timeout:
                        reply = None
                    if reply != expected_acks[index]:
                        raise AssertionError(f"Gói {index + 1}: ACK {reply!r}, mong đợi {expected_acks[index]!r}")
                    if reply is not None:
                        acks.append(reply.decode("ascii"))
            time.sleep(0.12)
            stop_monitor(process)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

    log = log_path.read_text(encoding="utf-8").replace("\r\n", "\n")
    counters = COUNTERS.findall(log)
    if not counters:
        raise AssertionError(f"Không đọc được bộ đếm trong {log_path.name}.")
    result = dict(zip(FIELDS, map(int, counters[-1])))
    if tuple(result.values()) != expected:
        raise AssertionError(f"{case}: bộ đếm thực nhận {result}, mong đợi {dict(zip(FIELDS, expected))}")
    with csv_path.open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
    accepted = [row for row in rows if row["Record_Type"] in ("UDP", "UDP_LATE")]
    if [int(row["Seq_Num"]) for row in accepted] != expected_seq:
        raise AssertionError(f"{case}: sai thứ tự hoặc thiếu hàng CSV sau khi dừng.")
    if any(None in row or None in row.values() for row in rows):
        raise AssertionError(f"{case}: số trường CSV không nhất quán.")

    result.update(sent=count_sent, datagrams_not_received=count_sent - result["received"],
                  accepted_seq=expected_seq, valid_sensor_unique=result["unique"] - result["invalid_sensor"])
    if case == "all":
        timestamp = lambda row: dt.datetime.fromisoformat(row["Timestamp_UTC"].replace("Z", "+00:00"))
        previous, resumed = timestamp(accepted[2]), timestamp(accepted[3])
        stale_rows = [row for row in rows if row["Sensor_Status"] == "STALE" and previous < timestamp(row) < resumed]
        if not stale_rows or accepted[3]["Sensor_Status"] != "FRESH":
            raise AssertionError("Không quan sát được chuyển trạng thái STALE rồi FRESH.")
        for row in stale_rows:
            if row["Record_Type"] != "STATUS" or any(row[key] != "N/A" for key in (
                "Seq_Num", "Temperature_C", "Humidity_pctRH", "Sensor_Valid"
            )):
                raise AssertionError("Dòng STALE chứa dữ liệu cảm biến đã quá hạn.")
        result["first_stale_after_s"] = (timestamp(stale_rows[0]) - previous).total_seconds()
        result["sensor_gap_s"] = (resumed - previous).total_seconds()
        if result["first_stale_after_s"] < 3 or result["sensor_gap_s"] < 4.9:
            raise AssertionError("Khoảng chờ trong CSV ngắn hơn kịch bản.")
    elif case == "wrap":
        if accepted[2]["Record_Type"] != "UDP_LATE":
            raise AssertionError("Gói bù đến muộn cần có loại UDP_LATE.")
        invalid = accepted[3]
        if [invalid[key] for key in ("Sensor_Status", "Sensor_Valid", "Temperature_C", "Humidity_pctRH")] != ["INVALID", "0", "N/A", "N/A"]:
            raise AssertionError("Gói VALID:0 phải giữ seq và ghi N/A cho số đo.")
        if [accepted[4][key] for key in ("Temperature_C", "Humidity_pctRH", "Sensor_Valid")] != ["0", "0", "1"]:
            raise AssertionError("Giá trị 0 hợp lệ phải được giữ trong CSV.")
    elif case == "ack":
        sessions = [row["Sensor_Session"] for row in accepted]
        if sessions != ["0123456789abcdef"] * 4 + ["abcdef0123456789"] * 2:
            raise AssertionError("Phiên ESP32 mới phải chấp nhận seq từ đầu, bỏ qua phiên cũ.")
        if [row["Sensor_Uptime_ms"] for row in accepted] != ["2500"] * 6:
            raise AssertionError("CSV phải lưu thời điểm lấy mẫu trên ESP32.")
        if accepted[2]["Record_Type"] != "UDP_LATE" or accepted[3]["Sensor_Status"] != "INVALID":
            raise AssertionError("CSV phải ghi gói bù và số đo lỗi của firmware.")
        if f"ACK đã gửi {len(acks)}  |  Lỗi gửi ACK 0  |  Phiên ESP32 2" not in log:
            raise AssertionError("Dashboard phải thống kê ACK và hai phiên ESP32.")
        result.update(acks=acks, sensor_sessions=2)
    return result


def firmware_packet(session, seq, *, requires_ack=True, valid=True, humidity=60.0):
    return json.dumps({
        "type": "data", "session": session, "seq": seq, "uptime_ms": 2500,
        "temperature_c": 28.0 if valid else None, "humidity_pct": humidity if valid else None,
        "sensor_valid": valid, "ack_required": requires_ack,
    }, separators=(",", ":")).encode("ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="Đường dẫn tới chương trình C++ đã build")
    parser.add_argument("--output-dir", type=Path, required=True, help="Thư mục mới để lưu log")
    parser.add_argument("--udp-port", type=int, default=8080)
    args = parser.parse_args()
    if not 1 <= args.udp_port <= 65535:
        parser.error("--udp-port phải nằm trong 1..65535")
    binary = args.binary.absolute()
    if not binary.is_file():
        parser.error(f"Không tìm thấy chương trình: {binary}")
    destination = args.output_dir.absolute()
    destination.mkdir(parents=True, exist_ok=True)

    all_packets = [(event.delay, payload_for(event)) for event in events_for("all")]
    all_packets[0] = (0, all_packets[0][1])
    wrap_packets = [
        (0, b"SEQ:4294967294;TEMP:28;HUM:60;VALID:1"),
        (0.1, b"SEQ:0;TEMP:29;HUM:61;VALID:1"),
        (0.1, b"SEQ:4294967295;TEMP:28.5;HUM:60.5;VALID:1"),
        (0.1, b"SEQ:0;TEMP:29;HUM:61;VALID:1"),
        (0.1, b"SEQ:1;TEMP:nan;HUM:nan;VALID:0"),
        (0.1, b"SEQ:2;TEMP:0;HUM:0;VALID:1"),
    ]
    first, second = "0123456789abcdef", "abcdef0123456789"
    retry = firmware_packet(first, 10)
    ack_packets = [
        (0, retry),
        (0.1, retry),
        (0.1, firmware_packet(first, 12)),
        (0.1, firmware_packet(first, 11)),
        (0.1, firmware_packet(first, 13, valid=False)),
        (0.1, firmware_packet(second, 1, requires_ack=False)),
        (0.1, firmware_packet(first, 14)),
        (0.1, firmware_packet(second, 2, humidity=100.01)),
        (0.1, firmware_packet(second, 2) + b"\0garbage"),
        (0.1, firmware_packet(second, 2) + b" " * 256),
        (0.1, firmware_packet(second, 2)),
    ]
    expected_acks = [f"ACK,{first},{seq}".encode("ascii") for seq in (10, 10, 12, 11, 13)]
    expected_acks.extend([None] * 5 + [f"ACK,{second},2".encode("ascii")])
    results = {
        "source": "Windows/POSIX UDP loopback. Python sendto và bộ đếm/CSV của tiến trình C++.",
        "port": args.udp_port,
        "all": measure(binary, args.udp_port, destination, "all", all_packets,
                       (7, 5, 1, 1, 0, 2, 0, 0), [10, 13, 14, 15, 16]),
        "wrap": measure(binary, args.udp_port, destination, "wrap", wrap_packets,
                        (6, 5, 0, 1, 0, 0, 1, 1), [4294967294, 0, 4294967295, 1, 2]),
        "ack": measure(binary, args.udp_port, destination, "ack", ack_packets,
                       (11, 6, 3, 1, 1, 0, 1, 1), [10, 12, 11, 13, 1, 2], expected_acks),
    }
    (destination / "ket_qua_loopback.json").write_text(
        json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(results, ensure_ascii=True, indent=2))


if __name__ == "__main__":
    main()
