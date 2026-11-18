"""Doi chieu cung (session,seq) giua sender.csv va bo nhan CUOI; tao report.csv/json."""
import argparse
import csv
import json
import math
import statistics
import sys
from pathlib import Path

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')

FINAL = {'ACKED', 'NO_ACK', 'WIFI_LOST', 'LOCAL_FAIL', 'UNCONFIRMED', 'PACKET_TOO_LONG'}
def read_csv(path):
    with path.open(newline='', encoding='utf-8-sig') as stream:
        return list(csv.DictReader(stream))
def key(row):
    return row['session'], int(row['seq'])
def yes(value):
    return str(value).lower() in ['1', 'true']
def latency(values):
    if not values:
        return {'n': 0, 'mean': None, 'median': None, 'p95': None, 'max': None}
    values = sorted(values)
    return {'n': len(values), 'mean': round(statistics.mean(values), 2),
            'median': statistics.median(values), 'p95': values[math.ceil(.95 * len(values)) - 1],
            'max': max(values)}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sender', type=Path, required=True, help='Duong dan sender.csv')
    parser.add_argument('--receiver', type=Path, required=True, help='Thu muc --out cua may nhan cuoi')
    parser.add_argument('--out', type=Path, help='Mac dinh: thu muc receiver/report')
    args = parser.parse_args()
    meta = json.loads((args.receiver / 'receiver_metadata.json').read_text(encoding='utf-8'))
    if meta['role'] != 'receiver':
        parser.error('Hay dung so lieu may Khoa/bo nhan CUOI, khong dung CSV cua relay')
    sender = read_csv(args.sender)
    packets = read_csv(args.receiver / 'receiver.csv')
    measurements = read_csv(args.receiver / 'measurements.csv')
    created = {key(row) for row in sender if row['event'] == 'CREATED' and row.get('in_window') != '0'}
    if not created:
        parser.error('Khong co CREATED trong cua so do. Kiem tra firmware ACK va bo thu Serial.')
    finished = {key(row): row for row in sender if row['event'] in FINAL and key(row) in created}
    delivered = {key(row): row for row in measurements if key(row) in created}
    attempts = {(key(row), int(row['attempt'])): row for row in sender if row['event'] == 'TRY' and key(row) in created}
    acked = [row for row in finished.values() if row['event'] == 'ACKED']
    failed = [row for row in finished.values() if row['event'] not in ['ACKED', 'UNCONFIRMED']]
    unconfirmed = [row for row in finished.values() if row['event'] == 'UNCONFIRMED']
    in_window = [row for row in sender if row.get('in_window') != '0']
    skipped_wifi = sum(row['event'] == 'SKIP_WIFI' for row in in_window)
    skipped_busy = sum(row['event'] == 'SKIP_BUSY' for row in in_window)
    skipped_config = sum(row['event'] == 'SKIP_CONFIG' for row in in_window)
    samples = len(created) + skipped_wifi + skipped_busy + skipped_config
    modes = {yes(row['ack_enabled']) for row in in_window if row.get('ack_enabled') != ''}
    gaps = []
    received_times = sorted(int(row['recv_elapsed_ms']) for row in delivered.values())
    for earlier, later in zip(received_times, received_times[1:]): gaps.append(later - earlier)
    disconnects, restored = {}, {}
    reconnect_ms, first_ack_after_reconnect_ms = [], []
    for row in in_window:
        event, session = row['event'], row['session']
        if not row.get('uptime_ms'): continue
        uptime = int(row['uptime_ms'])
        if event == 'WIFI_DOWN': disconnects[session] = uptime
        if event == 'WIFI_UP' and session in disconnects:
            reconnect_ms.append((uptime - disconnects.pop(session)) % (2**32))
            restored[session] = uptime
        if event == 'ACKED' and session in restored:
            first_ack_after_reconnect_ms.append((uptime - restored.pop(session)) % (2**32))
    result = {
        'mode': 'ACK' if modes == {True} else ('UDP' if modes == {False} else 'MIXED'),
        'sessions': sorted({session for session, _ in created}),
        'logical_frames': len(created), 'received_unique': len(delivered),
        'delivery_pct': round(100 * len(delivered) / len(created), 2),
        'acked_frames': len(acked), 'ack_pct': round(100 * len(acked) / len(created), 2),
        'failed_frames': len(failed), 'unconfirmed_frames': len(unconfirmed),
        'unfinished_frames': len(created - finished.keys()),
        'attempts_observed': len(attempts),
        'local_udp_success_observed': sum(yes(row['local_ok']) for row in attempts.values()),
        'retries_observed': sum(attempt > 1 for _, attempt in attempts),
        'duplicates_received': sum(key(row) in created and yes(row['duplicate']) for row in packets),
        'received_sensor_errors': sum(not yes(row['sensor_valid']) for row in delivered.values()),
        'samples_observed': samples, 'skipped_wifi': skipped_wifi, 'skipped_busy': skipped_busy,
        'skipped_config': skipped_config, 'sample_coverage_pct': round(100 * len(delivered) / samples, 2),
        'rtt_first_attempt_ms': latency([int(row['rtt_ms']) for row in acked if row.get('rtt_ms')]),
        'confirmation_ms': latency([int(row['confirm_ms']) for row in acked]),
        'max_receive_gap_ms': max(gaps) if gaps else None,
        'wifi_reconnect_ms': reconnect_ms,
        'first_ack_after_wifi_up_ms': first_ack_after_reconnect_ms,
        'receiver_run_finished': (args.receiver / 'receiver_summary.json').exists(),
        'simulated_loss': {name: meta[name] for name in ['drop_ack_every', 'drop_data_every', 'drop_ack_all', 'ack_delay_ms']},
    }
    output = args.out or args.receiver / 'report'
    output.mkdir(parents=True, exist_ok=True)
    (output / 'report.json').write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
    with (output / 'report.csv').open('w', newline='', encoding='utf-8-sig') as stream:
        writer = csv.writer(stream); writer.writerow(['metric', 'value'])
        for name, value in result.items():
            writer.writerow([name, json.dumps(value, ensure_ascii=False) if isinstance(value, (list, dict)) else value])
    print(f"Che do: {result['mode']}")
    print(f"Ban tin tao moi: {len(created)}; nhan duy nhat: {len(delivered)} ({result['delivery_pct']}%)")
    print(f"ESP32 nhan ACK: {len(acked)} ({result['ack_pct']}%); that bai: {len(failed)}; chua ket thuc: {result['unfinished_frames']}")
    print(f"Lan thu gui: {len(attempts)}; gui lai: {result['retries_observed']}; ban trung: {result['duplicates_received']}")
    print(f"So do bo qua khi mat Wi-Fi: {skipped_wifi}; phu so do: {result['sample_coverage_pct']}%")
    print(f"RTT lan dau: {result['rtt_first_attempt_ms']}; thoi gian xac nhan: {result['confirmation_ms']}")
    print(f"Bao cao: {output / 'report.csv'}")
    if result['unfinished_frames'] or not result['receiver_run_finished']:
        print('LUU Y: Luot do con ban tin chua chot hoac bo nhan chua dung; xem day la ket qua tam thoi.')

if __name__ == '__main__':
    main()
