"""Independent UART safety monitor and optional DPS student supply."""
import json
import os
import struct
import sys
import threading
import time

import serial
from serial.tools import list_ports

PSU_APP = r"D:\Downloads\学生电源Windows上位机"
if os.path.isfile(os.path.join(PSU_APP, "protocol.py")):
    sys.path.insert(0, PSU_APP)
    from protocol import FrameParser, PowerState, READ, WRITE, SESSION, make_frame, float_payload


def ports(vid_pid):
    return [p.device for p in list_ports.comports() if (p.vid, p.pid) in vid_pid]


class UartMonitor:
    def __init__(self, port=None):
        candidates = [port] if port else ports({(0x34B7, 0x6877), (0x1A86, 0x7523)})
        if len(candidates) != 1:
            raise RuntimeError(f"CH340 串口不唯一或未连接：{candidates}；请指定 --uart")
        self.port = serial.Serial(candidates[0], 2000000, timeout=0.05, write_timeout=1)
        self.running = True
        self.buffer = bytearray()
        self.latest = None
        self.log = None
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        while self.running:
            try:
                self.buffer.extend(self.port.read(self.port.in_waiting or 1))
                while len(self.buffer) >= 64:
                    end = self.buffer.find(b"\x00\x00\x80\x7f")
                    if end < 0:
                        del self.buffer[:-63]
                        break
                    if end < 60:
                        del self.buffer[:end + 4]
                        continue
                    start = end - 60
                    if start:
                        del self.buffer[:start]
                    row = struct.unpack_from("<15f", self.buffer)
                    del self.buffer[:64]
                    state = int(row[14])
                    sample = {"host_ns": time.monotonic_ns(), "seq": int(row[4]),
                              "rpm": row[5], "bus_v": row[6], "iq_ref": row[7],
                              "state": state & 7, "fault": (state >> 3) & 15}
                    self.latest = sample
                    if self.log and sample["seq"] % 100 == 0:
                        with self.lock:
                            if self.log:
                                self.log.write(json.dumps(sample) + "\n")
            except (OSError, serial.SerialException):
                self.running = False

    def start_log(self, path):
        with self.lock:
            self.log = open(path, "w", encoding="utf-8", buffering=1)

    def stop_log(self):
        with self.lock:
            if self.log:
                self.log.close()
                self.log = None

    def send(self, command):
        self.port.write(command.encode("ascii") + b"\r\n")

    def health(self, limit=8000.0):
        sample = self.latest
        if not self.running or not sample or time.monotonic_ns() - sample["host_ns"] > 500_000_000:
            raise RuntimeError("CH340 监测中断")
        if sample["fault"]:
            raise RuntimeError(f"MCU 保护故障 {sample['fault']}")
        if abs(sample["rpm"]) >= limit:
            raise RuntimeError(f"转速达到监测上限：{sample['rpm']:.0f} rpm")
        return sample

    def close(self):
        self.running = False
        self.thread.join(timeout=1)
        self.stop_log()
        self.port.close()


class StudentPower:
    def __init__(self, port):
        if "PowerState" not in globals():
            raise RuntimeError(f"缺少学生电源协议：{PSU_APP}\\protocol.py")
        self.port = serial.Serial(port, 9600, timeout=0.05, write_timeout=1)
        self.state = PowerState()
        self.parser = FrameParser()
        self.lock = threading.Lock()
        self.log = None
        self.last_rx = 0.0
        self.last_full_ns = 0
        self.commands = []
        self.running = True
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()
        self.send(SESSION, 0x00, b"\x01")
        for command in (0xDE, 0xDF, 0xFF):
            self.send(READ, command)
        try:
            self._wait(lambda: self.last_full_ns and self.state.model.startswith("DPS"), 2.0)
        except Exception:
            self.close()
            raise

    def send(self, kind, command, payload=b"\x00"):
        with self.lock:
            self.port.write(make_frame(kind, command, payload))

    def _read(self):
        next_poll = time.monotonic() + 0.5
        while self.running:
            try:
                if time.monotonic() >= next_poll:
                    self.send(READ, 0xFF)
                    next_poll = time.monotonic() + 0.5
                data = self.port.read(self.port.in_waiting or 1)
                if not data:
                    continue
                at = time.monotonic_ns()
                if self.log:
                    with self.lock:
                        if self.log:
                            self.log.write(json.dumps({"host_ns": at, "raw_chunk": data.hex()}) + "\n")
                for kind, command, payload in self.parser.feed(data):
                    self.state.update(command, payload)
                    self.last_rx = time.monotonic()
                    if command == 0xFF:
                        self.last_full_ns = at
                    if self.log:
                        with self.lock:
                            if self.log:
                                s = self.state
                                self.log.write(json.dumps({"host_ns": at,
                                    "command": command, "input_v": s.input_voltage,
                                    "set_v": s.set_voltage, "set_a": s.set_current,
                                    "output_v": s.output_voltage, "output_a": s.output_current,
                                    "output_w": s.output_power, "enabled": s.output_enabled,
                                    "protection": s.protection_status}) + "\n")
            except (OSError, serial.SerialException):
                self.running = False

    def _wait(self, predicate, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.05)
        raise RuntimeError("学生电源读回超时")

    def poll(self):
        self.send(READ, 0xFF)

    def configure(self, volts, amps=5.0):
        if volts > self.state.max_voltage or amps > self.state.max_current:
            raise RuntimeError("电源设定超过设备额定范围")
        before = self.last_full_ns
        self.send(WRITE, 0xC1, float_payload(volts))
        self.send(WRITE, 0xC2, float_payload(amps))
        self.poll()
        self._wait(lambda: self.last_full_ns > before and abs(self.state.set_voltage - volts) < .05 and
                   abs(self.state.set_current - amps) < .05, 2.0)

    def output(self, enabled):
        self.commands.append({"host_ns": time.monotonic_ns(), "output": enabled})
        print(f"脚本设置电源输出：{'开' if enabled else '关'}")
        before = self.last_full_ns
        self.send(WRITE, 0xDB, bytes((int(enabled),)))
        self.poll()
        self._wait(lambda: self.last_full_ns > before and self.state.output_enabled == enabled, 2.0)

    def health(self, require_output=True):
        if not self.running or time.monotonic_ns() - self.last_full_ns > 2_000_000_000:
            raise RuntimeError("学生电源通信中断")
        if self.state.protection_status:
            raise RuntimeError(f"学生电源保护 {self.state.protection_status}；输入 {self.state.input_voltage:.2f} V，输出 {self.state.output_voltage:.2f} V")
        if require_output and not self.state.output_enabled:
            raise RuntimeError("学生电源输出意外关闭（非本段脚本关断）")

    def start_log(self, path):
        with self.lock:
            self.log = open(path, "w", encoding="utf-8", buffering=1)

    def stop_log(self):
        with self.lock:
            if self.log:
                self.log.close()
                self.log = None

    def close(self):
        self.running = False
        self.thread.join(timeout=1)
        self.stop_log()
        self.port.close()
