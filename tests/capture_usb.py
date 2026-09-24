"""Validate one 20 kHz logging group of the 405_FOC USB stream.

Frame: 8 little-endian float32 + the JustFloat terminator = 36 bytes.
Channel 0 packs t_u24 with the status word, channel 1 packs the sample counter
with the group number; see tools/bench/README.md for the group channel maps.

pip install pyserial
"""
import argparse
import struct
import time

import serial

FRAME_BYTES = 36
CHANNELS = 8
TERMINATOR = b"\x00\x00\x80\x7f"
T_24_MASK = 0xFFFFFF
GROUP_NAMES = {i: name for i, name in enumerate(("raw0", "raw1", "current", "control", "voltage", "voltage2", "current2", "control2"))}


def capture(port, seconds, group):
    header = struct.Struct("<II")
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
        link.write(f"send {group}\r\n".encode("ascii"))
        last_data = time.monotonic()
        while started is None or time.monotonic() - started < seconds:
            data = link.read(max(1, min(link.in_waiting, 65536)))
            now = time.monotonic()
            if not data:
                if now - last_data > 2:
                    raise RuntimeError("USB stream stalled: inspect g_usb_stats; "
                                       "reset/replug after a latched fault")
                continue
            last_data = now
            pending.extend(data)
            if not synced:
                # Find a complete frame by its fixed 32-byte body plus terminator;
                # a JustFloat sequence may also occur inside payload floats.
                found = None
                for start in range(max(0, len(pending) - 4096), len(pending) - FRAME_BYTES + 1):
                    if pending[start + CHANNELS * 4:start + FRAME_BYTES] != TERMINATOR:
                        continue
                    words = header.unpack_from(pending, start)
                    status = words[0] >> 24
                    index = words[1]
                    if (index >> 24 == group and status & 0x78 == 0 and
                            (status & 7) <= 6):
                        found = start
                        break
                if found is None:
                    if len(pending) > 8192:
                        del pending[:-FRAME_BYTES]
                    continue
                del pending[:found]
                synced = True
            used = 0
            while len(pending) - used >= FRAME_BYTES:
                if bytes(pending[used + CHANNELS * 4:used + FRAME_BYTES]) != TERMINATOR:
                    raise RuntimeError(f"Frame boundary lost after {frames} frames")
                time_us, index = header.unpack_from(pending, used)
                if index >> 24 != group:
                    raise RuntimeError(f"Group changed mid-capture: {index >> 24} != {group}")
                time_us &= T_24_MASK
                seq = index & T_24_MASK
                if previous is not None:
                    previous_time, previous_seq = previous
                    if not 45 <= ((time_us - previous_time) & T_24_MASK) <= 55:
                        raise RuntimeError(f"Timing excursion: {previous_time} -> {time_us} us")
                    if ((seq - previous_seq) & T_24_MASK) != 1:
                        raise RuntimeError(f"Sample counter jumped: {previous_seq} -> {seq}")
                previous = (time_us, seq)
                used += FRAME_BYTES
                frames += 1
            del pending[:used]
            if frames and started is None:
                started = now
        elapsed = time.monotonic() - started
        rate = frames / elapsed
        if not 19800 <= rate <= 20200:
            raise RuntimeError(f"Continuous frames but unexpected rate: {rate:.1f} samples/s")
        print(f"PASS: group {group} ({GROUP_NAMES[group]}), {frames} consecutive frames, "
              f"{elapsed:.2f} s, {rate:.1f} samples/s, {frames * FRAME_BYTES / elapsed:.0f} bytes/s")
        link.dtr = False
        drain_until = time.monotonic() + 0.25
        while time.monotonic() < drain_until:
            link.read(max(1, link.in_waiting))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="Native USB CDC port, not the CH340 UART port")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--group", type=int, default=0, choices=sorted(GROUP_NAMES),
                        help="logging group to validate with `send X`")
    args = parser.parse_args()
    if args.seconds < 10:
        parser.error("Use at least 10 seconds for rate validation")
    capture(args.port, args.seconds, args.group)
