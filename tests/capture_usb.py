"""Validate the default 8-channel, 20 kHz USB JustFloat stream (pip install pyserial)."""
import argparse
import math
import struct
import time

import serial


def capture(port, seconds):
    frame = struct.Struct("<9f")
    tail = b"\x00\x00\x80\x7f"
    pending = bytearray()
    previous = None
    frames = 0
    started = None
    synced = False
    with serial.Serial() as link:
        link.port = port
        link.baudrate = 2000000  # CDC line coding only, not the USB bit rate.
        link.timeout = 0.1
        link.dtr = False
        link.open()
        # Finish any previous session while the MCU producer is stopped.
        drain_until = time.monotonic() + 0.25
        while time.monotonic() < drain_until:
            link.read(max(1, link.in_waiting))
        link.reset_input_buffer()
        link.dtr = True
        last_data = time.monotonic()
        while started is None or time.monotonic() - started < seconds:
            data = link.read(max(1, min(link.in_waiting, 65536)))
            now = time.monotonic()
            if not data:
                if now - last_data > 2:
                    raise RuntimeError("USB stream stalled: inspect g_usb_stats; reset/replug after a latched fault")
                continue
            last_data = now
            pending.extend(data)
            if not synced:
                # Opening an existing CDC session can expose an initial partial frame.
                pos = pending.find(tail)
                if pos < 0:
                    if len(pending) > 4096:
                        raise RuntimeError("No JustFloat boundary found")
                    continue
                del pending[:pos + 4]
                synced = True
            used = 0
            while len(pending) - used >= frame.size:
                if pending[used + 32:used + 36] != tail:
                    raise RuntimeError(f"Frame boundary lost after {frames} frames")
                values = frame.unpack_from(pending, used)
                timestamp = values[0]
                if not math.isfinite(timestamp) or timestamp != int(timestamp) or not 0 <= timestamp <= 0xFFFFFF:
                    raise RuntimeError(f"Invalid microsecond timestamp: {timestamp}")
                timestamp = int(timestamp)
                if previous is not None and not 45 <= ((timestamp - previous) & 0xFFFFFF) <= 55:
                    raise RuntimeError(f"Missing/reordered samples or timing excursion: {previous} -> {timestamp} us")
                previous = timestamp
                used += frame.size
                frames += 1
            del pending[:used]
            if frames and started is None:
                started = now
        elapsed = time.monotonic() - started
        rate = frames / elapsed
        if not 19800 <= rate <= 20200:
            raise RuntimeError(f"Continuous timestamps but unexpected rate: {rate:.1f} samples/s")
        print(f"PASS: {frames} consecutive frames, {elapsed:.2f} s, {rate:.1f} samples/s, "
              f"{frames * frame.size / elapsed:.0f} bytes/s")
        link.dtr = False
        drain_until = time.monotonic() + 0.25
        while time.monotonic() < drain_until:
            link.read(max(1, link.in_waiting))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="Native USB CDC port, not the CH340 UART port")
    parser.add_argument("--seconds", type=float, default=60)
    args = parser.parse_args()
    if args.seconds < 10:
        parser.error("Use at least 10 seconds for rate validation")
    capture(args.port, args.seconds)
