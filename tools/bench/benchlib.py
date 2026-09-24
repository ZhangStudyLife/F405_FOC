"""Host side of the 405_FOC bench data system.

One USB frame is 12 little-endian float32 plus the JustFloat terminator
``00 00 80 7F``, 52 bytes total, sent at 20 kHz. Channels 0 and 1 are packed
header words; the ten remaining channels depend on the logging group selected
with ``send X``. See README.md for the per-group channel map.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
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
DATA_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "data"))
SEVEN_ZIP = shutil.which("7z") or r"C:\Program Files\7-Zip\7z.exe"

# Channels 2..11 per logging group, exactly as App/Control/app.c fills them.
GROUP_CHANNELS = {
    0: ["adc_raw_b", "adc_raw_c", "adc_raw_bus", "angle_raw_deg", "angle_deg",
        "sampled_ccr_a", "sampled_ccr_b", "sampled_ccr_c", "b_offset", "c_offset"],
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
    if terminators_ok(body):
        words = body[:, :CHANNELS * 4].copy().view("<u4").reshape(-1, CHANNELS)
        dropped = len(raw) - usable
    else:
        # A damaged USB transfer can remove bytes inside one frame. Recover
        # later complete frames by their terminator and group, then let the
        # sequence/timestamp checks report the missing sample.
        mark = TERMINATOR_WORDS
        ends = np.flatnonzero((raw[:-3] == mark[0]) & (raw[1:-2] == mark[1]) &
                               (raw[2:-1] == mark[2]) & (raw[3:] == mark[3]))
        starts = ends[ends >= CHANNELS * 4] - CHANNELS * 4
        starts = starts[raw[starts + 7] == raw[offset + 7]]
        if len(starts) < 2:
            raise ValueError(f"{path}: no recoverable frames")
        body = raw[starts[:, None] + np.arange(CHANNELS * 4)]
        words = body.copy().view("<u4").reshape(-1, CHANNELS)
        dropped = len(raw) - len(starts) * FRAME_BYTES
    table = np.empty((words.shape[0], CHANNEL_COUNT), dtype=np.float32)
    table[:, :CHANNELS] = words.view(np.float32)
    if table.shape[1] > CHANNELS:
        table[:, CHANNELS:] = np.nan
    return table, dropped, words


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
        self._read_buffer = bytearray(65536)
        self.last_rx = time.monotonic()
        self.reader_error = None

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
        self.last_rx = time.monotonic()
        self.reader_error = None
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def _reader(self):
        write = self._file.write
        readinto = self.port.readinto
        try:
            while not self._stop.is_set():
                count = readinto(self._read_buffer)
                if count:
                    write(memoryview(self._read_buffer)[:count])
                    self.last_rx = time.monotonic()
        except (OSError, serial.SerialException) as error:
            self.reader_error = error

    def health(self):
        if self.reader_error:
            raise RuntimeError(f"USB 采集错误：{self.reader_error}")
        if time.monotonic() - self.last_rx > 0.5:
            raise RuntimeError("USB 采集超过 0.5 秒无帧")

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
    single packet per waypoint keeps repeated runs comparable.
    """

    def __init__(self, link):
        self.link = link
        self.jitter = []
        self.sent = []

    def run(self, plan, duration, tick=0.002, health=None):
        plan = sorted(plan, key=lambda item: item[0])
        started = time.monotonic()
        deadline = started + duration
        index = 0
        while True:
            now = time.monotonic()
            if now >= deadline:
                break
            if health and health() is False:
                break
            elapsed_ms = (now - started) * 1000.0
            while index < len(plan) and plan[index][0] <= elapsed_ms:
                self.link.send(plan[index][1])
                self.jitter.append(round(elapsed_ms - plan[index][0], 3))
                self.sent.append((time.monotonic_ns(), plan[index][1]))
                index += 1
            time.sleep(tick)


def wait_idle(link, timeout=8.0, threshold=20.0):
    """stop, then poll the stream until the encoder says the rotor is still."""
    link.send("stop")
    deadline = time.monotonic() + timeout
    last = None
    seen_at = time.monotonic()
    reopened = False
    while time.monotonic() < deadline:
        frames = link.listen(0.1)
        if not frames:
            if time.monotonic() - seen_at > 0.5:
                if reopened:
                    raise RuntimeError("USB telemetry stopped after reopening the session")
                link.close_session()
                link.open_session()
                link.send("send 3")
                link.send("stop")
                seen_at = time.monotonic()
                reopened = True
            continue
        seen_at = time.monotonic()
        last = frames[-1]
        if last.fault:
            raise RuntimeError(f"motor fault {FAULT_NAMES[last.fault]} while waiting for idle")
        if last.group == 3 and last.state == 0 and abs(last.channel(7)) < threshold:
            return True
    if last is None:
        raise RuntimeError("USB telemetry stopped while waiting for idle")
    raise RuntimeError(f"idle timeout: group={last.group} state={STATE_NAMES[last.state]} "
                       f"rpm={last.channel(7) if last.group == 3 else 'unavailable'}")


def archive(source, target, meta, verify=True):
    """Archive a complete segment; remove raw data only after verification."""
    directory = os.path.dirname(target)
    if directory:
        os.makedirs(directory, exist_ok=True)
    if target.endswith(".7z"):
        stage = os.path.dirname(source)
        names = ["frames.f32", "meta.json"]
        with open(os.path.join(stage, "meta.json"), "w", encoding="utf-8") as handle:
            json.dump(meta, handle, ensure_ascii=False, indent=2)
        names += [name for name in ("power.jsonl", "uart.jsonl", "failure.json") if os.path.exists(os.path.join(stage, name))]
        subprocess.run([SEVEN_ZIP, "a", "-t7z", "-m0=lzma2", "-mx=9", "-y", target, *names],
                       cwd=stage, check=True, stdout=subprocess.DEVNULL)
        if verify:
            subprocess.run([SEVEN_ZIP, "t", "-y", target], check=True, stdout=subprocess.DEVNULL)
        shutil.rmtree(stage)
    else:
        with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=1) as bundle:
            bundle.write(source, "frames.f32")
            bundle.writestr("meta.json", json.dumps(meta, indent=2, ensure_ascii=False))
        if verify:
            with zipfile.ZipFile(target) as bundle:
                stored = bundle.getinfo("frames.f32").file_size
                if stored != meta["raw_bytes"]:
                    raise RuntimeError(f"{target}: stored {stored} bytes, captured {meta['raw_bytes']}")
        os.remove(source)
    return target


def load(path):
    """Read new 7z segments and historical ZIPs without staging on C:."""
    os.makedirs(DATA_ROOT, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="read_", dir=DATA_ROOT) as directory:
        if path.endswith(".7z"):
            subprocess.run([SEVEN_ZIP, "e", "-y", "-o" + directory, path, "frames.f32", "meta.json"],
                           check=True, stdout=subprocess.DEVNULL)
            with open(os.path.join(directory, "meta.json"), encoding="utf-8") as handle:
                meta = json.load(handle)
            raw = os.path.join(directory, "frames.f32")
        else:
            with zipfile.ZipFile(path) as bundle:
                meta = json.loads(bundle.read("meta.json"))
                raw = bundle.extract("frames.f32", directory)
        table, dropped, words = parse_file(raw)
    meta["parsed_frames"] = int(len(table))
    meta["dropped_tail_bytes"] = int(dropped)
    meta["continuity"] = continuity(words, meta["group"]) if len(words) else {}
    meta["columns"] = meta.get("columns", COLUMNS[meta["group"]])
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
    index = {name: position for position, name in enumerate(meta["columns"])}
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
    os.makedirs(DATA_ROOT, exist_ok=True)
    path = os.path.join(DATA_ROOT, "foc_bench_probe.f32")
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
