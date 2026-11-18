#!/usr/bin/env python3
"""Gửi dữ liệu cảm biến ESP32 qua UDP loopback để kiểm thử bộ nhận C++."""

import argparse
import socket
import sys
import time
from itertools import count
from typing import Iterator, NamedTuple


DESTINATION = ("127.0.0.1", 8080)
INTERVAL = 1.0
TIMEOUT_PAUSE = 5.0
FIRST_SEQ = 10
UINT32_MASK = (1 << 32) - 1


class Event(NamedTuple):
    seq: int
    label: str = "BÌNH THƯỜNG"
    delay: float = INTERVAL
    malformed: bool = False


SCENARIOS = {
    "gap": (Event(10), Event(13, "BỎ QUA SEQ 11 VÀ 12"), Event(14)),
    "duplicate": (Event(10), Event(10, "TRÙNG"), Event(11)),
    "malformed": (Event(10), Event(11, "SAI CÚ PHÁP", malformed=True), Event(11)),
    "timeout": (Event(10), Event(11, "GỬI LẠI SAU KHI NGẮT", TIMEOUT_PAUSE), Event(12)),
    "all": (
        Event(10),
        Event(13, "BỎ QUA SEQ 11 VÀ 12"),
        Event(13, "TRÙNG"),
        Event(14, "SAI CÚ PHÁP", malformed=True),
        Event(14),
        Event(15, "GỬI LẠI SAU KHI NGẮT", TIMEOUT_PAUSE),
        Event(16),
    ),
}


def events_for(scenario: str) -> Iterator[Event]:
    if scenario == "normal":
        for seq in count(FIRST_SEQ):
            # ESP32 gửi số thứ tự uint32_t và quay về 0 khi tràn.
            yield Event(seq & UINT32_MASK)
    else:
        yield from SCENARIOS[scenario]


def payload_for(event: Event) -> bytes:
    # Cùng seq cho ra cùng chuỗi, vì vậy gói trùng giống hệt từng byte.
    step = event.seq % 20
    temperature = 26.0 + 0.2 * step
    humidity = 60.0 + 0.5 * step
    humidity_field = "HUM=" if event.malformed else "HUM:"
    text = f"SEQ:{event.seq};TEMP:{temperature:.1f};{humidity_field}{humidity:.1f};VALID:1"
    return text.encode("ascii")


def send_packets(scenario: str) -> None:
    print(f"Chế độ {scenario} • UDP {DESTINATION[0]}:{DESTINATION[1]} • Ctrl+C để dừng", flush=True)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        last_sent = None
        for event in events_for(scenario):
            if last_sent is not None:
                if event.delay == TIMEOUT_PAUSE:
                    print("Tạm ngắt gửi 5 giây để kiểm tra STALE.", flush=True)
                # Năm giây được tính từ gói trước, bao gồm chu kỳ 1 giây thông thường.
                deadline = last_sent + event.delay
                time.sleep(max(0.0, deadline - time.monotonic()))

            payload = payload_for(event)
            sock.sendto(payload, DESTINATION)
            last_sent = time.monotonic()
            print(f"[{time.strftime('%H:%M:%S')}] {event.label} | {payload.decode('ascii')}", flush=True)
    print("Đã gửi xong kịch bản. Xem bộ đếm và CSV trong chương trình C++.", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Giả lập ESP32 gửi UDP tới 127.0.0.1:8080 mỗi giây.",
        epilog=(
            "normal     Gửi liên tục từ seq 10 đến khi bấm Ctrl+C.\n"
            "gap        Nhảy từ seq 10 lên 13.\n"
            "duplicate  Gửi seq 10 hai lần với nội dung giống hệt.\n"
            "malformed  Gửi một gói có HUM= rồi gửi lại đúng cú pháp HUM:.\n"
            "timeout    Tạm dừng 5 giây rồi gửi tiếp.\n"
            "all        Chạy lần lượt tất cả các lỗi trong cùng một phiên."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--scenario", choices=("normal", *SCENARIOS), default="normal")
    args = parser.parse_args()

    try:
        send_packets(args.scenario)
    except KeyboardInterrupt:
        print("\nĐã dừng gửi UDP.", flush=True)
        return 0
    except OSError as error:
        print(f"Lỗi gửi UDP: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    # Giữ dấu tiếng Việt cả khi Windows chuyển đầu ra qua pipe hoặc tệp log.
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    sys.exit(main())
