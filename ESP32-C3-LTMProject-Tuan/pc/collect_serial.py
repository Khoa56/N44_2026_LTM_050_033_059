"""Ghi Serial ESP32 thanh sender.csv va sender_serial.log. Khong nap hay reset bo."""
import argparse
import csv
import json
import re
import sys
import time
from pathlib import Path

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')

FIELDS = ['host_epoch_ms', 'in_window', 'event', 'session', 'seq', 'uptime_ms',
          'ack_enabled', 'attempt', 'local_ok', 'confirm_ms', 'rtt_ms', 'samples_total',
          'logical_total', 'attempts_total', 'tx_ok_total', 'retries_total',
          'acked_total', 'failed_total', 'unconfirmed_total', 'skipped_wifi',
          'skipped_busy', 'skipped_config', 'wifi_disconnects', 'pending']
TRY = re.compile(r'^TX seq=(\d+) attempt=(\d+) local=(OK|FAIL)$')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--port', help='Vi du COM6; dong Serial Monitor truoc')
    source.add_argument('--input-log', type=Path, help='Chuyen nhat ky Serial co san thanh CSV')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--duration', type=float, default=120, help='Cua so do chinh, giay; 0 = den Ctrl+C')
    parser.add_argument('--grace', type=float, default=3, help='Ghi them de ban tin cu kip nhan ACK')
    args = parser.parse_args()
    if not (0 <= args.duration <= 86400 and 0 <= args.grace <= 60):
        parser.error('duration 0..86400; grace 0..60')
    args.out.mkdir(parents=True, exist_ok=True)
    for name in ['sender.csv', 'sender_serial.log', 'sender_capture.json']:
        if (args.out / name).exists():
            parser.error(f'{name} da ton tai; chon thu muc moi de khong tron luot do')
    serial_port = None
    if args.port:
        try:
            import serial
        except ImportError:
            parser.error('Hay chay bang Python cua PlatformIO (.platformio/penv/Scripts/python.exe)')
        serial_port = serial.Serial(port=None, baudrate=115200, timeout=.25)
        serial_port.dtr = False
        serial_port.rts = False
        serial_port.port = args.port
        try:
            serial_port.open()
        except serial.SerialException as error:
            parser.error(f'Khong mo duoc {args.port}. Hay dong Serial Monitor va kiem tra cong USB. {error}')
    start = time.monotonic()
    metadata = {'start_epoch_ms': int(time.time() * 1000), 'duration_s': args.duration,
                'grace_s': args.grace, 'port': args.port, 'metric_rows': 0, 'bad_metric_lines': 0}
    current_session = ''
    try:
        with (args.out / 'sender.csv').open('x', newline='', encoding='utf-8-sig') as csv_file, \
             (args.out / 'sender_serial.log').open('x', encoding='utf-8') as raw:
            writer = csv.DictWriter(csv_file, fieldnames=FIELDS, extrasaction='ignore')
            writer.writeheader()

            def record(line):
                nonlocal current_session
                raw.write(line + '\n'); raw.flush()
                row = None
                if line.startswith('METRIC '):
                    try:
                        row = json.loads(line[len('METRIC '):])
                        if not isinstance(row, dict) or not isinstance(row.get('session'), str):
                            raise ValueError('missing session')
                        current_session = row['session']
                    except (ValueError, TypeError):
                        metadata['bad_metric_lines'] += 1
                        return
                else:
                    match = TRY.fullmatch(line)
                    if match and current_session:
                        row = {'event': 'TRY', 'session': current_session, 'seq': int(match[1]),
                               'attempt': int(match[2]), 'local_ok': match[3] == 'OK'}
                if row is None:
                    return
                row['host_epoch_ms'] = int(time.time() * 1000)
                row['in_window'] = int(not args.port or not args.duration or time.monotonic() - start < args.duration)
                writer.writerow(row); csv_file.flush()
                metadata['metric_rows'] += 1
                if row.get('event') in ['ACKED', 'NO_ACK', 'UNCONFIRMED', 'WIFI_DOWN', 'WIFI_UP']:
                    print(f"{row.get('event')} seq={row.get('seq')} attempt={row.get('attempt')}", flush=True)

            print('READY SERIAL. Nhan RESET tren ESP32 de bat dau mot session moi.', flush=True)
            if args.input_log:
                for line in args.input_log.read_text(encoding='utf-8-sig', errors='replace').splitlines():
                    record(line.strip())
            else:
                pending = b''
                while not args.duration or time.monotonic() - start < args.duration + args.grace:
                    pending += serial_port.read(4096)
                    while b'\n' in pending:
                        line, pending = pending.split(b'\n', 1)
                        record(line.decode('utf-8', errors='replace').strip())
    except KeyboardInterrupt:
        print('\nDa dung ghi Serial.')
    finally:
        if serial_port:
            serial_port.close()
        metadata['end_epoch_ms'] = int(time.time() * 1000)
        (args.out / 'sender_capture.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    print(f"DONE: {args.out / 'sender.csv'}; rows={metadata['metric_rows']}")

if __name__ == '__main__':
    main()
