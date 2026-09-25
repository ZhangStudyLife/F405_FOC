"""Validate the 2 kHz complete telemetry stream of the 405_FOC USB device.

Frame: 15 little-endian float32 + the JustFloat terminator = 64 bytes.
Channel 0 packs t_u24 with the status word; channel 1 contains the 20 kHz sample
counter with its upper byte fixed to zero.

pip install pyserial
"""
import argparse
import struct
import time

import serial

CHANNEL_BYTES = 15 * 4
FRAME_BYTES = CHANNEL_BYTES + 4
TERMINATOR = b"\x00\x00\x80\x7f"
T_24_MASK = 0xFFFFFF


def capture(port, seconds):
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
                    if pending[start + CHANNEL_BYTES:start + FRAME_BYTES] != TERMINATOR:
                        continue
                    words = header.unpack_from(pending, start)
                    status = words[0] >> 24
                    index = words[1]
                    if (index >> 24 == 0 and status & 0x78 == 0 and
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
                if bytes(pending[used + CHANNEL_BYTES:used + FRAME_BYTES]) != TERMINATOR:
                    raise RuntimeError(f"Frame boundary lost after {frames} frames")
                time_us, index = header.unpack_from(pending, used)
                if index >> 24 != 0:
                    raise RuntimeError(f"Unexpected header byte: {index >> 24}")
                time_us &= T_24_MASK
                seq = index & T_24_MASK
                if previous is not None:
                    previous_time, previous_seq = previous
                    if not 450 <= ((time_us - previous_time) & T_24_MASK) <= 550:
                        raise RuntimeError(f"Timing excursion: {previous_time} -> {time_us} us")
                    if ((seq - previous_seq) & T_24_MASK) != 10:
                        raise RuntimeError(f"Sample counter jumped: {previous_seq} -> {seq}")
                previous = (time_us, seq)
                used += FRAME_BYTES
                frames += 1
            del pending[:used]
            if frames and started is None:
                started = now
        elapsed = time.monotonic() - started
        rate = frames / elapsed
        if not 1980 <= rate <= 2020:
            raise RuntimeError(f"Continuous frames but unexpected rate: {rate:.1f} samples/s")
        print(f"PASS: complete telemetry, {frames} consecutive frames, "
              f"{elapsed:.2f} s, {rate:.1f} samples/s, {frames * FRAME_BYTES / elapsed:.0f} bytes/s")
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
