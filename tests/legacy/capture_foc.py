# Historical voltage-mode protocol only; do not use with current firmware.
"""Capture 48-byte FOC JustFloat frames; optional bounded voltage trial.

python tests/capture_foc.py --seconds 5 --uq 0.3 --out build/foc/run_030_final
Requires pyserial. A trial always sends stop and waits for gate-off telemetry.
"""
import argparse
import csv
import json
import math
from pathlib import Path
import struct
import time

import serial

FIELDS = "time_ms duty_a duty_b duty_c angle_deg b_voltage c_voltage rpm uq state fault".split()
TAIL = b"\x00\x00\x80\x7f"


def decode(data):
    rows, offsets = [], []
    i = 0
    while i + 48 <= len(data):
        if data[i + 44:i + 48] == TAIL:
            row = struct.unpack_from("<11f", data, i)
            valid = (math.isfinite(row[0]) and row[0] >= 0 and row[0].is_integer()
                     and all(0 <= x <= 1 for x in row[1:4])
                     and row[9] in range(6) and row[10] in range(8))
            # USB receive startup may contain partial/garbled bytes. Require
            # two consecutive timestamps before accepting the initial alignment.
            aligned = rows or (i + 96 <= len(data) and data[i + 92:i + 96] == TAIL
                               and struct.unpack_from("<f", data, i + 48)[0] - row[0] == 1)
            if valid and aligned:
                rows.append(row)
                offsets.append(i)
                i += 48
                continue
        i += 1
    return rows, offsets


def save(prefix, data):
    prefix = Path(prefix)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    prefix.with_suffix(".bin").write_bytes(data)
    rows, offsets = decode(data)
    with prefix.with_suffix(".csv").open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(FIELDS)
        writer.writerows(rows)
    summary = {"frames": len(rows), "states": sorted(set((r[9], r[10]) for r in rows))}
    if rows:
        summary.update(first=rows[0], last=rows[-1],
                       timestamp_gaps=sum(b[0] - a[0] != 1 for a, b in zip(rows, rows[1:])),
                       internal_byte_gaps=sum(b - a != 48 for a, b in zip(offsets, offsets[1:])),
                       initial_skipped_bytes=offsets[0],
                       rpm_range=[min(r[7] for r in rows), max(r[7] for r in rows)])
    prefix.with_suffix(".json").write_text(json.dumps(summary, indent=2))
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM14")
    parser.add_argument("--seconds", type=float, default=5)
    parser.add_argument("--uq", type=float, help="Optional run voltage; omission is passive capture")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or not 0 < args.seconds <= 60:
        parser.error("seconds must be in (0, 60]")
    if args.uq is not None and (not math.isfinite(args.uq) or abs(args.uq) > 0.6):
        parser.error("uq must be finite and within +/-0.6 V")
    data = bytearray()
    with serial.Serial(args.port, 2000000, timeout=0.02) as port:
        port.reset_input_buffer()
        # Discard USB-open garbage; establish a valid idle link before a trial.
        startup = bytearray()
        end = time.monotonic() + 0.15
        while time.monotonic() < end:
            startup.extend(port.read(port.in_waiting or 48))
        initial, _ = decode(startup)
        if args.uq is not None and (not initial or initial[-1][9:11] != (0, 0)):
            raise RuntimeError("Trial requires valid IDLE telemetry with no fault")
        if args.uq is not None:
            port.write(f"\nrun {args.uq}\n".encode())
            port.flush()
        start = time.monotonic()
        try:
            while time.monotonic() - start < args.seconds:
                data.extend(port.read(port.in_waiting or 48))
        finally:
            if args.uq is not None:
                stopped = False
                for _ in range(3):
                    port.write(b"\nstop\n")
                    port.flush()
                    reply = bytearray()
                    end = time.monotonic() + 0.25
                    while time.monotonic() < end:
                        reply.extend(port.read(port.in_waiting or 48))
                    data.extend(reply)
                    rows, _ = decode(reply)
                    if rows and rows[-1][9] in (0, 5) and rows[-1][1:4] == (0, 0, 0):
                        stopped = True
                        break
                if not stopped:
                    save(args.out, data)
                    raise RuntimeError("STOP NOT CONFIRMED: turn off motor supply and inspect UART")
    summary = save(args.out, data)
    print(json.dumps(summary))
    if args.uq is not None and (not any(state == 4 for state, _ in summary["states"])
                                or any(fault for _, fault in summary["states"])):
        raise RuntimeError("Trial did not enter RUN cleanly; inspect saved log")


if __name__ == "__main__":
    main()
