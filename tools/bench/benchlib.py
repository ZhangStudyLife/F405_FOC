"""Host side of the 405_FOC bench data system.

One USB frame is 12 little-endian float32 plus the JustFloat terminator
``00 00 80 7F``, 52 bytes total, sent at 20 kHz. Channels 0 and 1 are packed
header words; the ten remaining channels depend on the logging group selected
with ``send X``. See README.md for the per-group channel map.
"""
import argparse
import json
import math
import os
import struct
import sys
import tempfile
import threading
import time
import zipfile

import numpy as np
import serial
from serial.tools import list_ports

FRAME_BYTES = 52
CHANNELS = 12
TERMINATOR = b"\x00\x00\x80\x7f"
SAMPLE_HZ = 20000.0
SAMPLE_US = 50
T_24_MASK = 0xFFFFFF
SERIAL_SPEED = 2000000
USB_VID_PID = (0x0483, 0x5740)

# Channels 2..11 per logging group, exactly as App/Control/app.c fills them.
GROUP_CHANNELS = {
    0: ["adc_raw_b", "adc_raw_c", "adc_raw_bus", "v_b", "v_c", "v_bus",
        "angle_raw_deg", "sample_us", "bus_v_nominal", "b_offset"],
    1: ["ib", "ic", "elec_deg", "iq_ref", "integral_d", "integral_q",
        "ud", "uq", "b_offset", "c_offset"],
    2: ["ud", "uq", "ccr_a", "ccr_b", "ccr_c", "duty_a", "duty_b", "duty_c",
        "edge_limit_v", "vec_limit_v"],
    3: ["iq_ref", "iq_ref_cmd", "pos_deg", "pos_tgt", "rpm", "rpm_encoder",
        "rpm_tgt", "iq", "mode", "bus_v"],
}
CHANNEL_COUNT = 2 + 26  # union of the channels any group can carry


def _columns(group):
    """Matrix column names: header, then this group's ten live channels."""
    names = ["t_us", "seq"] + [f"g{group}_{n}" for n in GROUP_CHANNELS[group]]
    return names + [f"unused{n}" for n in range(len(names), CHANNEL_COUNT)]


COLUMNS = {group: _columns(group) for group in GROUP_CHANNELS}

GROUP_NAMES = {0: "raw", 1: "current", 2: "voltage", 3: "control"}

# FOC state machine and fault codes, App/FOC/foc.h.
STATE_NAMES = ["IDLE", "PRECHARGE", "CALIBRATE", "SAVE", "RUN", "FAULT", "OFFSET"]
FAULT_NAMES = ["OK", "SENSOR", "ADC", "TIMING", "WINDOW", "ALIGNMENT", "FLASH",
               "UART", "BUS", "ZERO", "CURRENT", "SPEED", "POSITION"]
MOTOR_NAMES = ["NOT_PWM", "PWM"]

# Raw ADC scaling, App/Hardware/bsp/bsp_adc.c. Nominal only: the current gain
# has never been calibrated against a reference, so treat amp values as
# provisional and use the raw codes for any offline re-scaling.
VSENSE_PER_CODE = 3.3 / 4095.0
BUS_DIVIDER = 41.2 / 2.2
CURRENT_PER_VOLT = 50.0


class Frame:
    """Decoded header words of a single frame."""

    __slots__ = ("time_us", "state", "fault", "power", "seq", "group", "body")

    def __init__(self, body):
        word0, word1 = struct.unpack_from("<II", body)
        self.time_us = word0 & T_24_MASK
        self.state = word0 >> 24 & 0x7
        self.fault = word0 >> 27 & 0xF
        self.power = word0 >> 31 & 0x1
        self.seq = word1 & T_24_MASK
        self.group = word1 >> 24 & 0xFF
        self.body = body

    def channel(self, index):
        """Channel value by its index inside the 12-float frame."""
        return struct.unpack_from("<f", self.body, index * 4)[0]

    def __repr__(self):
        return (f"Frame(t={self.time_us} seq={self.seq} group={self.group} "
                f"state={STATE_NAMES[self.state]} fault={FAULT_NAMES[self.fault]})")


class FrameReader:
    """Byte stream to frames, keeping a partial frame between calls."""

    def __init__(self, keep=4):
        self.pending = bytearray()
        self.keep = keep
        self.frames = 0
        self.skipped = 0

    def feed(self, data):
        self.pending.extend(data)
        out = []
        while len(self.pending) >= FRAME_BYTES:
            if bytes(self.pending[CHANNELS * 4:FRAME_BYTES]) != TERMINATOR:
                # Out of alignment (a session change can start mid-frame): drop a
                # byte and retest. The terminator is inside the frame payload too,
                # so searching for it would risk locking onto a false boundary,
                # while a wrong offset always exposes a byte just after it.
                del self.pending[:1]
                self.skipped += 1
                continue
            out.append(Frame(bytes(self.pending[:CHANNELS * 4])))
            del self.pending[:FRAME_BYTES]
        self.frames += len(out)
        return out

    def latest(self):
        """The most recent frame, or None when fewer than one is buffered."""
        if len(self.pending) < FRAME_BYTES:
            return None
        tail = bytes(self.pending[-FRAME_BYTES:])
        if tail[CHANNELS * 4:FRAME_BYTES] != TERMINATOR:
            return None
        return Frame(tail[:CHANNELS * 4])


TERMINATOR_WORDS = np.frombuffer(TERMINATOR, dtype=np.uint8)


def terminators_ok(body):
    """True when every row ends with the JustFloat terminator."""
    return len(body) > 0 and bool(np.all(body[:, CHANNELS * 4:FRAME_BYTES] == TERMINATOR_WORDS))


def parse_file(path):
    """Parse a raw capture into a float matrix plus continuity statistics.

    Captures begin wherever DTR was raised, which can be mid-frame, so the head
    is resynchronised on the first frame that ends with the terminator.
    """
    raw = np.fromfile(path, dtype=np.uint8)
    offset = 0
    while len(raw) - offset >= FRAME_BYTES:
        window = raw[offset + CHANNELS * 4:offset + FRAME_BYTES]
        if np.array_equal(window, TERMINATOR_WORDS):
            break
        offset += 1
    if len(raw) - offset < 2 * FRAME_BYTES:
        raise ValueError(f"{path}: no frame boundary found; wrong layout or empty capture")
    usable = ((len(raw) - offset) // FRAME_BYTES) * FRAME_BYTES
    body = raw[offset:offset + usable].reshape(-1, FRAME_BYTES)
    if not terminators_ok(body):
        raise ValueError(f"{path}: frame terminator lost; wrong layout or corrupt capture")
    words = body[:, :CHANNELS * 4].copy().view("<u4").reshape(-1, CHANNELS)
    table = np.empty((words.shape[0], CHANNEL_COUNT), dtype=np.float32)
    table[:, :CHANNELS] = words.view(np.float32)
    if table.shape[1] > CHANNELS:
        table[:, CHANNELS:] = np.nan
    return table, offset + len(raw) - offset - usable, words


def continuity(words, group):
    """Gap, sequence and status statistics for a single-group capture."""
    time_us = (words[:, 0] & T_24_MASK).astype(np.int64)
    seq = (words[:, 1] & T_24_MASK).astype(np.int64)
    groups = words[:, 1] >> 24 & 0xFF
    status = words[:, 0] >> 24 & 0xFF
    stats = {
        "frames": int(len(words)),
        "group_mismatch": int(np.count_nonzero(groups != group)),
        "faults": int(np.count_nonzero(status >> 3 & 0xF)),
        "max_state": int(np.max(status & 0x7)) if len(words) else 0,
    }
    if len(words) < 2:
        stats.update(gaps=0, max_gap_us=0, missing=0, seq_ok=True, dt_ok=True)
        return stats
    dt = (np.diff(time_us)) & T_24_MASK
    dseq = (np.diff(seq)) & T_24_MASK
    stats["gaps"] = int(np.count_nonzero((dt < 45) | (dt > 55) | (dseq != 1)))
    stats["timestamp_jitter"] = int(np.count_nonzero(dt != SAMPLE_US))
    stats["max_gap_us"] = int(dt.max())
    stats["missing"] = int(np.sum(dseq - 1))
    stats["seq_ok"] = bool(np.all(dseq == 1))
    stats["dt_ok"] = bool(stats["gaps"] == 0)
    return stats


def find_port(serial_number=None):
    """Locate the native CDC interface, never the CH340 or an ST-Link port."""
    matches = [p for p in list_ports.comports() if (p.vid, p.pid) == USB_VID_PID]
    if serial_number:
        matches = [p for p in matches if p.serial_number == serial_number]
    if not matches:
        raise RuntimeError("no 0483:5740 native USB CDC port found; is the board "
                           "plugged in with a data cable and the firmware running?")
    if len(matches) > 1:
        names = ", ".join(f"{p.device}(sn={p.serial_number})" for p in matches)
        raise RuntimeError(f"multiple FOC boards present, pass --sn: {names}")
    return matches[0]


class Link:
    """Serial link with a reader thread that only appends bytes to a file.

    Parsing during capture is deliberately avoided: at 1.04 MB/s any per-frame
    Python work would stall the host and break the 20 kHz record.
    """

    def __init__(self, port, sn=None, timeout=0.05):
        self.port = serial.Serial()
        self.port.port = port
        self.port.baudrate = SERIAL_SPEED
        self.port.timeout = timeout
        self.port.write_timeout = 1.0
        self.port.dtr = False
        self.port.rtscts = False
        self.port.open()
        self.serial_number = sn
        self._stop = threading.Event()
        self._path = None
        self._file = None
        self._thread = None

    def drain(self, seconds=0.25):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            n = self.port.in_waiting
            self.port.read(n if n else 1)

    def open_session(self):
        """Raise DTR so the firmware counts a session, after finishing the old."""
        self.drain()
        self.port.reset_input_buffer()
        self.port.dtr = True

    def close_session(self):
        self.drain()
        self.port.dtr = False

    def send(self, command):
        self.port.write(command.encode("ascii") + b"\r\n")

    def start_capture(self, path):
        self._path = path
        self._file = open(path, "wb", buffering=1024 * 1024)
        self._stop.clear()
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def _reader(self):
        write = self._file.write
        read = self.port.read
        while not self._stop.is_set():
            waiting = self.port.in_waiting
            data = read(waiting if waiting else 1)
            if data:
                write(data)

    def stop_capture(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2.0)
            self._thread = None
        if self._file:
            self._file.flush()
            os.fsync(self._file.fileno())
            self._file.close()
            self._file = None
        return self._path

    def listen(self, seconds):
        """Collect frames for a short interval; used for state polling."""
        pending = bytearray()
        latest = None
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            n = self.port.in_waiting
            data = self.port.read(n if n else 1)
            if data:
                pending.extend(data)
                end = pending.rfind(TERMINATOR)
                if end >= CHANNELS * 4:
                    latest = Frame(bytes(pending[end - CHANNELS * 4:end]))
                    del pending[:end + len(TERMINATOR)]
                elif len(pending) > FRAME_BYTES:
                    del pending[:-FRAME_BYTES]
        return [latest] if latest else []

    def close(self):
        try:
            self.stop_capture()
        finally:
            try:
                self.port.dtr = False
            except Exception:
                pass
            self.port.close()


class Scheduler:
    """Deterministic command timeline: identical wall-clock times every repeat.

    The value is what carries the waveform, not the exact instant it lands, so a
    single packet per waypoint with the firmware ramp in between keeps repeated
    runs comparable even when the host is jittery.
    """

    def __init__(self, link, keepalive_ms=100):
        self.link = link
        self.keepalive = keepalive_ms / 1000.0
        self.jitter = []

    def run(self, plan, duration, tick=0.002):
        plan = sorted(plan, key=lambda item: item[0])
        started = time.monotonic()
        deadline = started + duration
        index = 0
        current = None
        last_sent = -math.inf
        while True:
            now = time.monotonic()
            if now >= deadline:
                break
            elapsed_ms = (now - started) * 1000.0
            while index < len(plan) and plan[index][0] <= elapsed_ms:
                self.link.send(plan[index][1])
                self.jitter.append(round(elapsed_ms - plan[index][0], 3))
                current = plan[index][1]
                last_sent = now
                index += 1
            if current is not None and now - last_sent >= self.keepalive:
                self.link.send(current)
                last_sent = now
            time.sleep(tick)


def wait_idle(link, timeout=8.0, threshold=20.0):
    """stop, then poll the stream until the encoder says the rotor is still."""
    link.send("stop")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        frames = link.listen(0.1)
        if frames and frames[-1].group == 3 and frames[-1].state == 0 and abs(frames[-1].channel(7)) < threshold:
            return True
    return False


def archive(source, target, meta, verify=True):
    """Stream the temporary capture into a zip next to its metadata."""
    directory = os.path.dirname(target)
    if directory:
        os.makedirs(directory, exist_ok=True)
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=1) as bundle:
        bundle.write(source, "frames.f32")
        bundle.writestr("meta.json", json.dumps(meta, indent=2, ensure_ascii=False))
    if verify:
        with zipfile.ZipFile(target) as bundle:
            stored = bundle.getinfo("frames.f32").file_size
            if stored != meta["raw_bytes"]:
                raise RuntimeError(f"{target}: stored {stored} bytes, "
                                   f"captured {meta['raw_bytes']}")
    os.remove(source)
    return target


def load(path):
    """Return (matrix, metadata) from an archive, unpacking to a temp file."""
    with zipfile.ZipFile(path) as bundle, tempfile.TemporaryDirectory(prefix="foc_bench_") as directory:
        meta = json.loads(bundle.read("meta.json"))
        raw = bundle.extract("frames.f32", directory)
        table, dropped, words = parse_file(raw)
    meta["parsed_frames"] = int(len(table))
    meta["dropped_tail_bytes"] = int(dropped)
    meta["continuity"] = continuity(words, meta["group"]) if len(words) else {}
    meta["columns"] = COLUMNS[meta["group"]]
    return table, meta


def summarise(table, meta):
    """Per-case metrics from the reference and ground-truth channels."""
    group = meta["group"]
    stats = {
        "frames": int(table.shape[0]),
        "seconds": round(table.shape[0] / SAMPLE_HZ, 3),
        "group": group,
        "group_name": GROUP_NAMES[group],
    }
    if group != 3:
        stats["note"] = ("raw/current/voltage carry non-reconstructible physics; "
                         "tracking metrics need group 3")
        return stats
    index = {name: position for position, name in enumerate(COLUMNS[group])}
    for name in ("rpm", "rpm_tgt", "pos_deg", "pos_tgt", "iq_ref", "iq"):
        values = table[:, index[f"g{group}_{name}"]].astype(np.float64)
        finite = values[np.isfinite(values)]
        stats[name] = ({
            "min": float(finite.min()), "max": float(finite.max()),
            "mean": float(finite.mean()), "std": float(finite.std()),
        } if finite.size else None)
    if stats.get("pos_tgt"):
        error = table[:, index[f"g{group}_pos_tgt"]].astype(np.float64) \
            - table[:, index[f"g{group}_pos_deg"]].astype(np.float64)
        finite = error[np.isfinite(error)]
        stats["pos_error"] = ({"max_abs": float(np.abs(finite).max()),
                               "mean": float(finite.mean())} if finite.size else None)
    stats["faults"] = int(np.count_nonzero(table[:, 0].view("<u4") >> 27 & 0xF))
    return stats


def list_devices():
    rows = []
    for p in list_ports.comports():
        rows.append({
            "device": p.device,
            "vid": f"{p.vid:04X}" if p.vid else None,
            "pid": f"{p.pid:04X}" if p.pid else None,
            "serial": p.serial_number,
            "description": p.description,
            "is_foc": (p.vid, p.pid) == USB_VID_PID,
        })
    return rows


def discover(seconds=1.5, sn=None):
    """Probe the board: identity plus a short stream and layout check."""
    port = find_port(sn)
    link = Link(port.device, port.serial_number)
    path = os.path.join(tempfile.gettempdir(), "foc_bench_probe.f32")
    try:
        link.open_session()
        link.send("stop")
        link.send("send 0")
        link.listen(0.1)
        link.start_capture(path)
        time.sleep(seconds)
        link.stop_capture()
        link.close_session()
    finally:
        link.close()
    table, dropped, words = parse_file(path)
    os.remove(path)
    frames = len(table)
    result = {
        "port": port.device,
        "serial": port.serial_number,
        "bytes": frames * FRAME_BYTES,
        "frames": frames,
        "samples_per_s": round(frames / seconds, 1),
        "bytes_per_s": round(frames * FRAME_BYTES / seconds),
        "terminator_ok": True,
    }
    result["layout_ok"] = dropped < FRAME_BYTES * 2
    if frames:
        result["continuity"] = continuity(words, 0)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="enumerate serial ports")
    parser.add_argument("--discover", action="store_true", help="probe the FOC board")
    parser.add_argument("--parse", help="summarise one capture archive")
    parser.add_argument("--sn", help="USB serial number, when several boards are present")
    args = parser.parse_args()
    if args.list:
        print(json.dumps(list_devices(), indent=2, ensure_ascii=False))
        return 0
    if args.discover:
        print(json.dumps(discover(sn=args.sn), indent=2, ensure_ascii=False))
        return 0
    if args.parse:
        table, meta = load(args.parse)
        print(json.dumps(summarise(table, meta), indent=2, ensure_ascii=False))
        return 0
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
