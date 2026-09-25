"""Download a completed 20 kHz FOC_CAPTURE dump from the CH340 UART."""
import argparse
import csv
import os
import struct
import sys
import time

import serial
from serial.tools import list_ports

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchlib

UART_IDS = {(0x34B7, 0x6877), (0x1A86, 0x7523)}
MAGIC = benchlib.FOC3_HEADER.pack(0x33434F46, 0)[:4]
def find_port(port):
    ports = [p.device for p in list_ports.comports() if (p.vid, p.pid) in UART_IDS]
    if port:
        return port
    if len(ports) != 1:
        raise RuntimeError(f"CH340 串口不唯一或未连接：{ports}；请指定 --port")
    return ports[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="output .foc3 path; a matching .csv is also written")
    parser.add_argument("--port", help="CH340 UART port; discovered when unique")
    args = parser.parse_args()
    port = find_port(args.port)
    with serial.Serial(port, benchlib.SERIAL_SPEED, timeout=0.05, write_timeout=1) as link:
        link.reset_input_buffer()
        link.write(b"dump\r\n")
        pending = bytearray()
        deadline = time.monotonic() + 8.0
        header = None
        while time.monotonic() < deadline:
            pending.extend(link.read(4096))
            start = pending.find(MAGIC)
            if start >= 0 and len(pending) >= start + benchlib.FOC3_HEADER.size:
                header = bytes(pending[start:start + benchlib.FOC3_HEADER.size])
                del pending[:start + benchlib.FOC3_HEADER.size]
                break
        if header is None:
            raise RuntimeError("未收到 FOC3 数据；先用 FOC_CAPTURE 固件完成 capture 并停机")
        magic, count = benchlib.FOC3_HEADER.unpack(header)
        if magic != 0x33434F46 or not 0 < count <= 2048:
            raise RuntimeError(f"FOC3 头无效或记录数超限：{count}")
        size = count * benchlib.FOC3_RECORD.size
        while len(pending) < size and time.monotonic() < deadline:
            pending.extend(link.read(size - len(pending)))
        if len(pending) < size:
            raise RuntimeError(f"FOC3 数据不完整：{len(pending)}/{size} 字节")
    raw = header + pending[:size]
    with open(args.output, "wb") as handle:
        handle.write(raw)
    rows = benchlib.parse_foc_capture(args.output)
    csv_path = os.path.splitext(args.output)[0] + ".csv"
    first = rows[0][0]
    with open(csv_path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["time_us", *benchlib.FOC3_FIELDS])
        for row in rows:
            writer.writerow([((row[0] - first) & 0xFFFFFF) * 50, *row])
    print(f"收到 {count} 点 ({count / 20000:.3f} s)，保存 {args.output} 和 {csv_path}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, serial.SerialException) as exc:
        print(f"采集错误：{exc}")
        sys.exit(1)
