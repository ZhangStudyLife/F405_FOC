"""F405 电机台架：中文菜单、可复现工况、20 kHz 分组归档。"""
import argparse
import hashlib
import json
import math
import os
import shutil
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field

import numpy as np

import benchlib as bench
import devices

# Safety envelope. The board has no throttle of its own for these modes, so the
# host is the last line of defence; widen these only on purpose.
DEFAULT_IQ_LIMIT = 5.0
DEFAULT_RPM_LIMIT = 7000.0
DEFAULT_POS_LIMIT = 21600.0
RAMP_RATE = 10.0
WARMUP_MS = 300             # settle before the plan starts; 300 samples of 1 ms.


@dataclass
class Experiment:
    """One repeatable operating condition."""

    case: str
    mode: str
    duration_s: float
    plan: list = field(default_factory=list)  # [(t_ms, "Iq 0.20"), ...]
    targets: dict = field(default_factory=dict)
    note: str = ""
    setup: list = field(default_factory=list)
    initial_rpm: float = 0.0
    stop_at_rpm: float = 0.0

    def check(self, iq_limit, rpm_limit, pos_limit):
        """Reject a plan that leaves the safe envelope before it is ever sent."""
        for _, command in self.plan + [(0, command) for command in self.setup]:
            if not command.startswith(("Iq ", "rpm ", "pos ")):
                continue
            value = float(command.split()[1])
            if command.startswith("Iq ") and abs(value) > iq_limit:
                return f"{self.case}: Iq {value} exceeds the {iq_limit} A limit"
            if command.startswith("rpm ") and abs(value) > rpm_limit:
                return f"{self.case}: rpm {value} exceeds the {rpm_limit} RPM limit"
            if command.startswith("pos ") and abs(value) > pos_limit:
                return f"{self.case}: pos {value} exceeds the {pos_limit} deg limit"
        if not self.plan:
            return f"{self.case}: empty plan"
        return None


# ---------------------------------------------------------------- case library

def wave_plan(command, points):
    return [(int(t * 1000), f"{command} {value:.2f}") for t, value in points]


def torque_cases(volts=24.0):
    """Separate stationary breakaway from the moving-rotor identification."""
    cases = []
    for sign in (1, -1):
        direction = "正" if sign > 0 else "负"
        ramp = [(n * 100, f"Iq {sign * n / 10:.2f}") for n in range(51)]
        cases.append(Experiment(f"breakaway_{'p' if sign > 0 else 'n'}", "torque", 5.2,
                                ramp, {"max_iq": sign * 5.0}, f"{direction}向静止起动阈值",
                                stop_at_rpm=25.0))
    for initial in (-1000, -250, 250, 1000):
        for amps in (-5.0, -2.0, -1.0, -.5, -.25, .25, .5, 1.0, 2.0, 5.0):
            cases.append(Experiment(f"moving_{initial:+d}_{amps:+.2f}", "torque", 2.6,
                                    [(0, f"Iq {amps:.2f}"), (600, "stop")],
                                    {"initial_rpm": initial, "iq": amps}, "带初速力矩与滑行",
                                    initial_rpm=initial))
    for initial in (-4000, -1000, -250, 250, 1000, 4000):
        cases.append(Experiment(f"coast_{initial:+d}", "torque", 3.0, [(0, "stop")],
                                {"initial_rpm": initial}, "断电滑行", initial_rpm=initial))
    for frequency in (.2, .5, 1.0):
        points = [(n * .02, .5 * math.sin(2 * math.pi * frequency * n * .02))
                  for n in range(int(6 / .02))]
        cases.append(Experiment(f"torque_sine_{frequency:g}", "torque", 6.0,
                                wave_plan("Iq", points), {"amplitude_a": .5, "frequency_hz": frequency},
                                "旋转中正弦力矩", initial_rpm=250))
    cases.append(Experiment("torque_square", "torque", 6.0,
                            wave_plan("Iq", [(n * .5, .5 if n % 2 == 0 else -.5) for n in range(12)]),
                            {"amplitude_a": .5}, "旋转中正反方波", initial_rpm=250))
    return cases


SPEED_POINTS = (1, 5, 25, 100, 250, 500, 1000, 2000, 4000, 6000, 7000)


def speed_cases(volts=24.0):
    cases = []
    plan, cursor = [(0, "rpm 0")], .5
    for sign in (1, -1):
        for rpm in SPEED_POINTS:
            plan.append((int(cursor * 1000), f"rpm {sign * rpm}"))
            cursor += .8 if volts < 24 and rpm > 360 * volts * .9 else 1.5
        plan.append((int(cursor * 1000), "rpm 0"))
        cursor += 1.0
    cases.append(Experiment("speed_atlas", "speed", cursor + .3, plan,
                            {"points_rpm": list(SPEED_POINTS)}, "正负恒速、起步、跨零、停车"))
    cases.append(Experiment("speed_cross_zero", "speed", 8.0,
                            [(0, "rpm 0"), (500, "rpm 1000"), (3000, "rpm -2000"), (6000, "rpm 0")],
                            {"sequence": [0, 1000, -2000, 0]}, "跨零阶跃"))
    for rpm in (1, 5, 25):
        cases.append(Experiment(f"one_turn_{rpm}", "speed", 1.0 + 60.0 / rpm,
                                [(0, f"rpm {rpm}"), (int(60000 / rpm), "rpm 0")],
                                {"rpm": rpm, "revolutions": 1}, "低速整圈"))
    for rate in (250, 1000, 5000):
        points = [(n * .02, min(1000, rate * n * .02))
                  for n in range(int(1000 / rate / .02) + 1)]
        cases.append(Experiment(f"speed_ramp_{rate}", "speed", 1000 / rate + 2.0,
                                wave_plan("rpm", points), {"rate_rpm_s": rate}, "匀加速斜坡"))
    for frequency in (.2, .5, 1.0):
        points = [(n * .02, 1000 * math.sin(2 * math.pi * frequency * n * .02))
                  for n in range(int(6 / .02))]
        cases.append(Experiment(f"speed_sine_{frequency:g}", "speed", 6.0,
                                wave_plan("rpm", points), {"amplitude_rpm": 1000, "hz": frequency},
                                "正弦速度"))
    cases.append(Experiment("speed_square", "speed", 8.0,
                            [(n * 1000, f"rpm {1000 if n % 2 == 0 else -1000}") for n in range(8)],
                            {"amplitude_rpm": 1000}, "方波速度"))
    points = []
    for segment, (start, finish) in enumerate(((0, 1000), (1000, -2000), (-2000, 0))):
        for n in range(101):
            u = n / 100
            points.append((segment * 2 + 2 * u, start + (finish - start) * (3*u*u - 2*u*u*u)))
    cases.append(Experiment("speed_cubic", "speed", 6.5, wave_plan("rpm", points),
                            {"sequence": [0, 1000, -2000, 0]}, "三次曲线速度"))
    return cases


MOTION_PRESETS = ((500, 5000, 50000), (3000, 20000, 200000), (7000, 50000, 500000))
POSITION_SPANS = (30, 90, 180, 360, 720, 1800, 3600)


def position_cases(volts=24.0):
    cases = []
    for index, (rpm, accel, jerk) in enumerate(MOTION_PRESETS, 1):
        plan, cursor = [], .5
        for span in POSITION_SPANS:
            for target in (span, 0, -span, 0):
                plan.append((int(cursor * 1000), f"pos {target}"))
                cursor += max(.75, 2 * span / (rpm * 6) + .7)
        cases.append(Experiment(f"position_profile_{index}", "position", cursor + .5,
                                plan, {"spans_deg": list(POSITION_SPANS), "motion": (rpm, accel, jerk)},
                                "多圈往返和中途重规划",
                                setup=[f"motion {rpm} {accel} {jerk}"]))
    rpm, accel, jerk = MOTION_PRESETS[-1]
    cases.append(Experiment("position_long_60turn", "position", 18.0,
                            [(0, "pos 21600"), (8000, "pos -21600"), (16000, "pos 0")],
                            {"motion": (rpm, accel, jerk), "optional": True}, "高速长行程专项",
                            setup=[f"motion {rpm} {accel} {jerk}"]))
    return cases


MODES = {"speed": speed_cases, "position": position_cases}
MODE_ZH = {"speed": "速度", "position": "位置"}


# ------------------------------------------------------------------ execution

def wait_speed(link, monitor, target, timeout=8.0, supply=None):
    link.send(f"rpm {target:.2f}")
    deadline = time.monotonic() + timeout
    stable = None
    while time.monotonic() < deadline:
        if supply:
            supply.health()
        frames = link.listen(.02)  # Drain the active 20 kHz stream during spin-up too.
        if monitor:
            sample = monitor.health()
            actual = sample["rpm"]
        else:
            if not frames:
                continue
            frame = frames[-1]
            if frame.fault:
                raise RuntimeError(f"MCU 保护故障 {bench.FAULT_NAMES[frame.fault]}")
            if frame.group != 3:
                continue
            actual = frame.channel(7)
        if abs(actual - target) <= max(5.0, .1 * abs(target)):
            stable = stable or time.monotonic()
            if time.monotonic() - stable >= .2:
                return
        else:
            stable = None
    raise RuntimeError(f"预转速未达到 {target:g} rpm，动态辨识已跳过")


def assess(table, meta):
    if meta["group"] != 3 or meta["mode"] == "torque":
        return "已采集"
    index = {name: position for position, name in enumerate(meta["columns"])}
    target_name = "rpm_tgt" if meta["mode"] == "speed" else "pos_tgt"
    actual_name = "rpm" if meta["mode"] == "speed" else "pos_deg"
    target = table[:, index[f"g3_{target_name}"]] if f"g3_{target_name}" in index else np.zeros(len(table))
    actual = table[:, index[f"g3_{actual_name}"]]
    if len(table) < 10000:
        return "数据不足"
    failures = 0
    evaluated = 0
    limited = 0
    changes = np.r_[0, np.where(np.abs(np.diff(target)) > .01)[0] + 1, len(target)]
    for start, stop in zip(changes[:-1], changes[1:]):
        wanted = float(target[start])
        if (meta["mode"] == "speed" and meta.get("bus_set_v", 24) < 24 and
                abs(wanted) > meta["bus_set_v"] * 360 * .9):
            limited += 1
            continue
        if stop - start < 10000:
            continue
        evaluated += 1
        if meta["mode"] == "speed":
            if wanted == 0:
                continue
            error = abs(float(np.mean(actual[stop-10000:stop])) - wanted)
            if error > (2.0 if abs(wanted) < 25 else abs(wanted) * .05):
                failures += 1
        else:
            error = np.abs(actual[stop-10000:stop] - wanted)
            rpm = np.abs(table[stop-10000:stop, 6])
            if np.any(error > 2.0) or np.any(rpm > 5.0):
                failures += 1
    if failures:
        return "跟踪失败"
    if limited:
        return "物理限幅响应"
    return "通过" if evaluated else "已采集"


def alignment(stage):
    path = os.path.join(stage, "uart.jsonl")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as handle:
        samples = [json.loads(line) for line in handle]
    if len(samples) < 2:
        return None
    first = samples[0]["seq"]
    seq = np.array([(row["seq"] - first) & 0xffffff for row in samples], dtype=np.int64)
    host = np.array([row["host_ns"] for row in samples], dtype=np.int64)
    offsets = host - seq * 50000
    anchor = int(np.median(offsets))
    return {"first_uart_seq": first, "host_ns_at_first_seq": anchor,
            "ns_per_sample": 50000, "uart_arrival_p95_ms":
            round(float(np.percentile(np.abs(offsets - anchor), 95)) / 1e6, 3),
            "psu_current_note": "约 2 Hz 母线电流趋势，不是相电流或高频测量"}


def run_case(link, out_dir, experiment, group, limits, monitor, supply=None,
             voltage=24.0, repeat=0, attempt=0, load=None):
    problem = experiment.check(*limits)
    if problem:
        raise RuntimeError(problem)
    started = time.monotonic()
    name = f"{experiment.case}_{voltage:g}V_g{group}_r{repeat}_a{attempt}"
    stage = os.path.join(out_dir, name)
    os.makedirs(stage, exist_ok=False)
    raw = os.path.join(stage, "frames.f32")
    archive = os.path.join(out_dir, name + ".7z")
    scheduler = bench.Scheduler(link)
    error = None
    capturing = False
    try:
        link.open_session()
        link.send("send 3")
        link.listen(.15)
        bench.wait_idle(link)
        if experiment.mode == "position":
            link.send("zero")
            link.listen(.1)
        for command in experiment.setup:
            link.send(command)
        if experiment.initial_rpm:
            wait_speed(link, monitor, experiment.initial_rpm, supply=supply)
        link.send(f"send {group}")
        link.drain(.15)
        if monitor:
            monitor.start_log(os.path.join(stage, "uart.jsonl"))
        if supply:
            supply.start_log(os.path.join(stage, "power.jsonl"))
        link.start_capture(raw)
        capturing = True
        time.sleep(WARMUP_MS / 1000)
        def health():
            link.health()
            if monitor:
                sample = monitor.health()
                if experiment.stop_at_rpm and abs(sample["rpm"]) >= experiment.stop_at_rpm:
                    return False
            if supply:
                supply.health()
        scheduler.run(experiment.plan, experiment.duration_s, health=health)
        link.send("stop")
        time.sleep(.2)
    except Exception as exc:
        error = str(exc)
        with open(os.path.join(stage, "failure.json"), "w", encoding="utf-8") as handle:
            json.dump({"host_ns": time.monotonic_ns(), "error": error,
                       "uart": monitor.latest if monitor else None,
                       "power": asdict(supply.state) if supply else None},
                      handle, ensure_ascii=False, indent=2)
    finally:
        try:
            if monitor:
                monitor.send("stop")
            link.send("stop")
        except (OSError, RuntimeError):
            pass
        if error and supply:
            try:
                supply.output(False)
            except (OSError, RuntimeError) as exc:
                error += f"；停机电源读回失败：{exc}"
        if capturing:
            link.stop_capture()
        if monitor:
            monitor.stop_log()
        if supply:
            supply.stop_log()
        try:
            link.close_session()
        except (OSError, RuntimeError):
            pass
    if not os.path.exists(raw) or os.path.getsize(raw) < 104:
        raise RuntimeError(error or "没有采到完整 USB 帧")
    meta = {
        "schema": 2, "case": experiment.case, "mode": experiment.mode,
        "group": group, "group_name": bench.GROUP_NAMES[group], "repeat": repeat,
        "attempt": attempt, "bus_set_v": voltage, "load": load or {"name": "空载"},
        "note": experiment.note, "targets": experiment.targets,
        "duration_s": experiment.duration_s, "elapsed_s": round(time.monotonic() - started, 3),
        "warmup_ms": WARMUP_MS, "ramp_rate_a_per_s": RAMP_RATE,
        "plan": [[int(t), c] for t, c in experiment.plan], "setup": experiment.setup,
        "initial_rpm": experiment.initial_rpm, "sent": scheduler.sent,
        "command_jitter_ms": scheduler.jitter, "raw_bytes": os.path.getsize(raw),
        "limits": {"iq": limits[0], "rpm": limits[1], "pos": limits[2]},
        "columns": bench.COLUMNS[group], "archive": os.path.basename(archive),
        "host": time.strftime("%Y-%m-%d %H:%M:%S"), "git": git_revision(),
        "firmware_sha256": firmware_hash(),
        "control_params": {"speed_kp": .005, "speed_ki": .01, "position_kp": 4.0,
                           "torque_ramp_a_s": 10.0, "foc_rpm_filter_alpha_20khz": .01},
        "psu": {"model": supply.state.model, "serial": supply.state.serial_number} if supply else None,
        "time_alignment": alignment(stage),
        "error": error,
    }
    try:
        _, _, words = bench.parse_file(raw)
        meta["continuity"] = bench.continuity(words, group)
    except ValueError as exc:
        meta["continuity"] = {"error": str(exc)}
        error = error or str(exc)
        meta["error"] = error
    bench.archive(raw, archive, meta)
    table, loaded = bench.load(archive)
    flags = loaded["continuity"]
    if flags.get("faults") or not flags.get("dt_ok", False):
        error = error or f"采样故障：丢帧 {flags.get('gaps')}，故障帧 {flags.get('faults')}"
    result = assess(table, loaded) if not error else "故障"
    print(f"\r  [完成] {MODE_ZH[experiment.mode]}/{experiment.case} 组{group} 第{repeat+1}轮 "
          f"{len(table)}帧，原始 {meta['raw_bytes']/1e6:.2f} MB → "
          f"{os.path.getsize(archive)/1e6:.2f} MB，耗时 {time.monotonic()-started:.2f} 秒，{result}")
    if error:
        raise RuntimeError(error)
    return result


def git_revision():
    try:
        import subprocess
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        out = subprocess.run(["git", "-C", root, "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=5)
        return out.stdout.strip() or None
    except Exception:
        return None


def firmware_hash():
    path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..",
                                 "build", "Release", "405_FOC.bin"))
    if not os.path.isfile(path):
        return None
    with open(path, "rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def reset_board(args):
    script = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "download", "flash.py"))
    command = [sys.executable, script, "reset"]
    if args.stlink_sn:
        command += ["--sn=" + args.stlink_sn]
    subprocess.run(command, check=True, timeout=30)
    time.sleep(2)


def select_supply(args):
    if args.psu == "off":
        return None
    candidates = [args.psu] if args.psu else devices.ports({(0x2E3C, 0x5740)})
    if not candidates:
        return None
    found = []
    for port in candidates:
        try:
            found.append(devices.StudentPower(port))
        except (OSError, RuntimeError):
            pass
    if len(found) == 1:
        return found[0]
    if len(found) > 1:
        names = [f"{power.port.port} {power.state.model}" for power in found]
        if args.yes:
            for power in found:
                power.close()
            raise RuntimeError(f"多个学生电源 {names}，请指定 --psu")
        print("检测到多个学生电源：" + ", ".join(names))
        selected = input("请输入本次台架电源端口：").strip().upper()
        chosen = next((power for power in found if power.port.port == selected), None)
        for power in found:
            if power is not chosen:
                power.close()
        return chosen
    raise RuntimeError("学生电源候选均无法通信，禁止启动电机")


def preflight(link, monitor, supply=None):
    link.open_session()
    try:
        link.send("send 3")
        link.listen(.15)
        bench.wait_idle(link)
        wait_speed(link, monitor, 250.0, timeout=8.0, supply=supply)
        return True
    finally:
        if monitor:
            monitor.send("stop")
        link.send("stop")
        link.close_session()


def recover(link, monitor, supply, args, voltage, error):
    print(f"故障：{error}；正在停机并尝试一次恢复")
    try:
        if monitor:
            monitor.send("stop")
    except Exception:
        pass
    try:
        link.send("stop")
    except Exception:
        pass
    if supply:
        try:
            supply.output(False)
        except (OSError, RuntimeError):
            port, commands = supply.port.port, supply.commands
            supply.close()
            supply.__init__(port)
            supply.commands = commands
            supply.output(False)
        if supply.state.protection_status:
            raise RuntimeError(f"电源保护未消失：{supply.state.protection_status}")
    # Reconnect the port too: a dead OS handle cannot be repaired with DTR.
    if link:
        link.close()
    fresh = None
    try:
        for reset in (False, True):
            if reset:
                if not supply:
                    raise RuntimeError("不能核实功率断电，禁止自动 ST-Link 复位")
                reset_board(args)
            try:
                port = bench.find_port(args.sn)
                fresh = bench.Link(port.device, port.serial_number)
                fresh.open_session()
                fresh.send("stop")
                fresh.send("send 3")
                frames = fresh.listen(.6)
                if not frames or frames[-1].group != 3:
                    raise RuntimeError("重连后没有组 3 状态")
                fresh.close_session()
                break
            except (OSError, RuntimeError):
                if fresh:
                    fresh.close()
                    fresh = None
                if reset:
                    raise
        if supply:
            supply.configure(voltage, 5.0)
            supply.output(True)
            time.sleep(.5)
            supply.health()
        # Clear requires a valid bus. Clearing while the supply is off is rejected.
        fresh.open_session()
        frames = fresh.listen(.2)
        if not frames:
            raise RuntimeError("恢复母线后 USB 无反馈")
        if frames[-1].fault:
            print(f"恢复检查：{bench.FAULT_NAMES[frames[-1].fault]}，尝试一次 clear")
            fresh.send("clear")
        bench.wait_idle(fresh, threshold=5.0)
        fresh.close_session()
        if monitor:
            if not monitor.running:
                port = monitor.port.port
                monitor.close()
                monitor.__init__(port)
                time.sleep(.2)
            monitor.health()
        print("恢复自检通过：有新帧、无故障、转子静止；重跑当前段")
        return fresh
    except Exception:
        if fresh:
            fresh.close()
        raise


def command_run(args):
    if not args.all and not args.mode and not args.case:
        if args.dry_run:
            args.all = True
        else:
            return menu(args)
    modes = list(MODES) if args.all else ([args.mode] if args.mode else list(MODES))
    voltages = (24.0, 18.0, 12.0) if args.all else tuple(float(v) for v in args.buses.split(","))
    limits = (args.iq_limit, args.rpm_limit, args.pos_limit)
    groups = [int(g) for g in args.groups.split(",")]
    if any(g not in range(8) for g in groups) or args.repeat < 1:
        raise RuntimeError("日志组只能为 0–3，重复次数至少为 1")
    work = []
    for voltage in voltages:
        for mode in modes:
            source = [args.custom_experiment] if getattr(args, "custom_experiment", None) else MODES[mode](voltage)
            for experiment in source:
                if experiment.mode != mode:
                    continue
                if experiment.targets.get("optional") and not args.include_long:
                    continue
                if args.case and experiment.case != args.case:
                    continue
                if experiment.check(*limits):
                    continue
                work.append((voltage, experiment))
    if not work:
        raise RuntimeError("没有匹配的工况；先用 run --all --dry-run 查看名称")
    if args.uart and args.uart.lower() in ("off", "none") and not args.dry_run:
        raise RuntimeError("电机工况需要 CH340 在线监测；off 不能用于电机运行")
    total = sum(case.duration_s + (5 if case.initial_rpm else 1.5) for _, case in work) * len(groups) * args.repeat
    print(f"{len(work)} 个工况 × {len(groups)} 组 × {args.repeat} 次，预计约 {total/60:.1f} 分钟")
    if args.dry_run:
        for voltage, case in work:
            print(f"  {voltage:g} V {MODE_ZH[case.mode]} {case.case} {case.duration_s:.1f} 秒 {case.note}")
        return 0
    if not args.yes and input("开始测试？输入 y 确认：").strip().lower() != "y":
        return 1
    root = os.path.abspath(args.out)
    out_dir = os.path.join(root, time.strftime("%Y%m%d_%H%M%S"))
    os.makedirs(out_dir, exist_ok=False)
    session = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "directory": out_dir,
               "groups": groups, "voltages": voltages, "git": git_revision(),
               "load": {"name": args.load_name, "mass_g": args.load_mass_g,
                        "stl": args.load_stl, "axis": args.load_axis}, "results": []}
    def save():
        with open(os.path.join(out_dir, "session.json"), "w", encoding="utf-8") as handle:
            json.dump(session, handle, ensure_ascii=False, indent=2)
    save()
    supply = None
    monitor = None
    link = None
    started = time.monotonic()
    failed = False
    try:
        supply = select_supply(args)
        if supply:
            session["power_commands"] = supply.commands
            session["power_initial"] = asdict(supply.state)
        if not supply and len(voltages) > 1:
            raise RuntimeError("多母线全量测试需要指定并连接学生电源")
        if not args.uart or args.uart.lower() not in ("off", "none"):
            monitor = devices.UartMonitor(args.uart)
        try:
            port = bench.find_port(args.sn)
            link = bench.Link(port.device, port.serial_number)
        except (OSError, RuntimeError) as exc:
            link = recover(None, monitor, supply, args, voltages[0], exc)
        print(f"FOC {link.port.port}，CH340 {monitor.port.port if monitor else '关闭'}，电源 {supply.port.port if supply else '未连接'}")
        current_voltage = None
        rotation_ok = None
        for voltage, experiment in work:
            if voltage != current_voltage:
                if supply:
                    supply.output(False)
                    supply.configure(voltage, 5.0)
                    supply.output(True)
                    time.sleep(.5)
                    supply.health()
                current_voltage = voltage
                rotation_ok = None
            if experiment.mode != "torque" or experiment.initial_rpm:
                if rotation_ok is None:
                    try:
                        rotation_ok = preflight(link, monitor, supply)
                    except Exception as exc:
                        session["results"].append({"voltage": voltage, "case": "preflight",
                            "status": "故障", "error": str(exc)})
                        save()
                        if "预转速未达到" in str(exc):
                            rotation_ok = False
                            print(f"起转自检失败：{exc}；本电压下动态、速度与位置工况跳过")
                        else:
                            link = recover(link, monitor, supply, args, voltage, exc)
                            rotation_ok = preflight(link, monitor, supply)
                if not rotation_ok:
                    session["results"].append({"voltage": voltage, "case": experiment.case,
                                               "status": "未执行", "reason": "起转自检失败"})
                    save()
                    continue
            for group in groups:
                for repeat in range(args.repeat):
                    for attempt in range(2):
                        try:
                            result = run_case(link, out_dir, experiment, group, limits, monitor,
                                              supply, voltage, repeat, attempt, session["load"])
                            session["results"].append({"voltage": voltage, "case": experiment.case,
                                "group": group, "repeat": repeat, "status": result, "attempt": attempt})
                            break
                        except Exception as exc:
                            session["results"].append({"voltage": voltage, "case": experiment.case,
                                "group": group, "repeat": repeat, "status": "故障", "error": str(exc),
                                "attempt": attempt, "host_ns": time.monotonic_ns(),
                                "uart": dict(monitor.latest) if monitor and monitor.latest else None,
                                "power": asdict(supply.state) if supply else None})
                            save()
                            if attempt:
                                failed = True
                                print(f"本段重试仍失败，已保存残段（{exc}）；跳过本段继续后续工况")
                                break
                            link = recover(link, monitor, supply, args, voltage, exc)
                    save()
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        failed = True
        print(f"测试中止：{exc}")
    except KeyboardInterrupt:
        failed = True
        print("用户中止，正在停机")
    finally:
        if monitor:
            try:
                monitor.send("stop")
            except Exception:
                pass
        if link:
            try:
                link.send("stop")
            except Exception:
                pass
            link.close()
        if supply:
            supply_port = supply.port.port
            try:
                supply.output(False)
            except Exception as exc:
                print(f"电源关闭读回超时：{exc}；重新连接核实")
            try:
                supply.close()
            except Exception:
                pass
            try:
                check = devices.StudentPower(supply_port)
                if check.state.output_enabled:
                    check.output(False)
                print(f"电源最终状态：输出={'开' if check.state.output_enabled else '关'}，保护={check.state.protection_status}")
                check.close()
            except Exception as exc:
                print(f"无法重新核实电源状态：{exc}")
        if monitor:
            monitor.close()
        session["elapsed_s"] = round(time.monotonic() - started, 2)
        session["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
        save()
        print(f"全程耗时 {session['elapsed_s']:.2f} 秒；数据：{out_dir}")
    return int(failed)


def command_report(args):
    files = sorted(f for f in os.listdir(args.directory) if f.endswith((".zip", ".7z")))
    if not files:
        raise SystemExit(f"no captures in {args.directory}")
    lines = [f"# 台架数据报告", "",
             f"- 目录：`{os.path.abspath(args.directory)}`",
             f"- 文件：{len(files)}", ""]
    rows = []
    mechanics = {}
    for name in files:
        table, meta = bench.load(os.path.join(args.directory, name))
        stats = bench.summarise(table, meta)
        flags = meta.get("continuity", {})
        rows.append((meta["case"], meta["group"], stats, flags))
        if (meta.get("group") == 3 and meta.get("mode") == "torque" and
                meta.get("case", "").startswith(("moving_", "coast_")) and
                not flags.get("gaps") and len(table) > 2000):
            # Decimate to 200 Hz, smooth encoder speed, and differentiate over
            # 50 ms. Static breakaway samples must never enter this fit.
            rpm = table[::100, 6].astype(float)
            iq = table[::100, 7].astype(float)
            smooth = np.convolve(rpm, np.ones(9) / 9, mode="same")
            omega = smooth[5:-5] * (math.pi / 30)
            domega = (smooth[10:] - smooth[:-10]) * (math.pi / 30) / .05
            current = iq[5:-5]
            ok = (np.isfinite(omega) & np.isfinite(domega) & np.isfinite(current) &
                  (np.abs(omega) > 25 * math.pi / 30) & (np.abs(omega) < 7000 * math.pi / 30))
            bus = float(meta.get("bus_set_v", np.nanmedian(table[::100, 11])
                        if table.shape[1] > 11 else np.nan))
            mechanics.setdefault(bus, []).append((current[ok], omega[ok], domega[ok]))
        print(f"{name}: {stats['frames']} frames, {stats['seconds']} s")
    lines += ["## 连续性", "",
              "| 文件 | 帧数 | 秒 | 丢帧 | 最大间隔 us | 序号连续 | 故障帧 |",
              "|---|---|---|---|---|---|---|"]
    for case, group, stats, flags in rows:
        lines.append(f"| {case}_g{group} | {stats['frames']} | {stats['seconds']} | "
                     f"{flags.get('missing', '-')} | {flags.get('max_gap_us', '-')} | "
                     f"{flags.get('seq_ok', '-')} | {flags.get('faults', '-')} |")
    lines += ["", "## 跟踪（仅组 3 有目标/机械通道）", "",
              "| 文件 | rpm 均值 | rpm 标准差 | 位置误差峰 | 位置误差均值 | Iq 均值 |",
              "|---|---|---|---|---|---|"]
    for case, group, stats, _ in rows:
        if not stats.get("pos_tgt"):
            continue
        error = stats.get("pos_error") or {}
        lines.append(f"| {case}_g{group} | {stats['rpm']['mean']:.1f} | "
                     f"{stats['rpm']['std']:.1f} | {error.get('max_abs', float('nan')):.2f} | "
                     f"{error.get('mean', float('nan')):.2f} | {stats['iq_ref']['mean']:.3f} |")
    lines += ["", "## 空载运动模型", "", "仅使用组 3 的旋转中力矩和滑行段，|rpm| > 25；"
              "速度平滑后以 50 ms 差分求加速度。系数次序为 Iq、ω、sgn(ω)、常数；"
              "单位依次为 A、rad/s、无量纲、rad/s²。静止起动数据未参与拟合。", ""]
    for bus, chunks in sorted(mechanics.items(), reverse=True):
        current, omega, domega = (np.concatenate(parts) for parts in zip(*chunks))
        if len(current) < 100 or np.ptp(current) < .1 or np.min(omega) >= 0 or np.max(omega) <= 0:
            lines.append(f"- {bus:g} V：旋转样本或正反方向不足，未拟合。")
            continue
        design = np.column_stack((current, omega, np.sign(omega), np.ones(len(omega))))
        coefficient = np.linalg.lstsq(design, domega, rcond=None)[0]
        residual = domega - design @ coefficient
        r2 = 1 - float(np.dot(residual, residual) / np.sum((domega - domega.mean()) ** 2))
        lines.append(f"- {bus:g} V：dω/dt = {coefficient[0]:.2f} Iq "
                     f"{coefficient[1]:+.4f} ω {coefficient[2]:+.2f} sgn(ω) "
                     f"{coefficient[3]:+.2f}；R²={r2:.4f}，样本 {len(current)}。")
        if abs(bus - 24) < .5:
            lines.append("  24 V 原模型：1230 Iq − 0.0654 ω − 192.5 sgn(ω) − 0.27；"
                         "仅与同母线实测结果比较。")
    target = os.path.join(args.directory, "REPORT.md")
    with open(target, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    print(f"wrote {target}")
    return 0


def command_chain(args):
    """Place independent repeats beside each other by capture-relative sample."""
    files = sorted(f for f in os.listdir(args.directory)
                   if f.startswith(args.case) and f.endswith((".zip", ".7z")))
    if len(files) < 2:
        raise SystemExit(f"need the same case on several groups, found {files}")
    blocks = []
    columns = ["capture_s"]
    seen = set()
    for name in files:
        table, meta = bench.load(os.path.join(args.directory, name))
        words = np.frombuffer(table[:, :12].tobytes(), dtype="<u4").reshape(-1, 12)
        seq = (words[:, 1] & 0xFFFFFF).astype(np.int64)
        group = int(words[0, 1] >> 24)
        if group in seen or meta["continuity"]["gaps"]:
            raise SystemExit(f"{name}: duplicate group or discontinuous capture")
        seen.add(group)
        blocks.append(np.column_stack((seq, table[:, 2:12])).astype(np.float64))
        columns += [f"g{group}_seq"] + meta["columns"][2:12]
    length = min(len(block) for block in blocks)
    matrix = np.column_stack([np.arange(length) / bench.SAMPLE_HZ] + [block[:length] for block in blocks])
    stem = os.path.join(args.directory, f"{args.case}_merged")
    np.savetxt(stem + ".csv", matrix, delimiter=",", header=",".join(columns),
               comments="", fmt="%.6g")
    print(f"wrote {stem}.csv  ({matrix.shape[0]} rows x {matrix.shape[1]} columns); independent runs aligned by capture start only")
    return 0


def command_discover(args):
    print(json.dumps(bench.discover(sn=args.sn), indent=2, ensure_ascii=False))
    return 0


def command_list(args):
    for row in bench.list_devices():
        mark = "*" if row["is_foc"] else " "
        print(f"{mark} {row['device']:<8} {row['vid']}:{row['pid']}  "
              f"{str(row['serial']):<20} {row['description']}")
    return 0


def custom_case(mode, shape):
    command = {"torque": "Iq", "speed": "rpm", "position": "pos"}[mode]
    default = {"torque": .5, "speed": 1000, "position": 360}[mode]
    value = float(input(f"{MODE_ZH[mode]}目标幅值（A/rpm/度）[{default}]：") or default)
    seconds = float(input("持续时间（秒）[6]：") or "6")
    if seconds <= 0:
        raise ValueError("持续时间必须大于 0")
    frequency = float(input("频率 Hz [直接回车为 0.5]：") or ".5") if shape in ("正弦", "方波") else .5
    points = []
    for n in range(int(seconds / .02) + 1):
        t = n * .02
        u = min(1.0, t / seconds)
        if shape == "固定":
            target = value
        elif shape == "阶跃":
            target = value if t >= .3 else 0.0
        elif shape == "斜坡":
            target = value * (2*u if u <= .5 else 2*(1-u))
        elif shape == "正弦":
            target = value * math.sin(2*math.pi*frequency*t)
        elif shape == "方波":
            target = value if math.sin(2*math.pi*frequency*t) >= 0 else -value
        else:
            x = 2*u if u <= .5 else 2*(1-u)
            target = value * (3*x*x - 2*x*x*x)
        points.append((n * 20, f"{command} {target:.2f}"))
    initial = float(input("力矩测试预转速 rpm [回车为 0，即静止起步]：") or "0") if mode == "torque" else 0.0
    setup = ["motion 500 5000 50000"] if mode == "position" else []
    return Experiment(f"custom_{mode}_{shape}_{time.strftime('%H%M%S')}", mode, seconds + .5,
                      points, {"amplitude": value, "frequency_hz": frequency}, shape,
                      setup=setup, initial_rpm=initial)


def menu(args):
    while True:
        print("\n====== F405 电机台架 ======\n1. 速度测试\n2. 位置测试\n"
              "3. 全量测试\n4. 设备与电源\n5. 数据报告\n0. 退出")
        choice = input("请选择：").strip()
        if choice == "0":
            return 0
        if choice in ("1", "2"):
            mode = {"1": "speed", "2": "position"}[choice]
            print("1. 综合预设  2. 固定  3. 阶跃  4. 斜坡  5. 正弦  6. 方波  7. 三次曲线")
            print("8. 特项列表（起动阈值/低速整圈/长行程等）  0. 返回")
            kind = input("请选择工况：").strip()
            if kind == "0":
                continue
            args.all = False
            args.mode = mode
            args.case = None
            args.custom_experiment = None
            if kind in ("2", "3", "4", "5", "6", "7"):
                args.custom_experiment = custom_case(mode, {
                    "2": "固定", "3": "阶跃", "4": "斜坡", "5": "正弦",
                    "6": "方波", "7": "三次曲线"}[kind])
            elif kind == "8":
                available = MODES[mode](24)
                for index, experiment in enumerate(available, 1):
                    print(f"{index:2}. {experiment.case}：{experiment.note}")
                selected = int(input("输入编号："))
                args.case = available[selected - 1].case
                args.include_long = True
            elif kind != "1":
                print("无效选项")
                continue
            args.buses, args.groups, args.repeat = "24", "0,1,2,3,4,5,6,7", 1
            print("使用默认方案：24 V、八组日志、每组一次；综合预设自动覆盖该模式全部档位。")
            try:
                command_run(args)
            except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
                print(f"台架错误：{exc}")
        elif choice == "3":
            args.all, args.mode, args.case, args.custom_experiment = True, None, None, None
            args.buses = "24,18,12"
            try:
                command_run(args)
            except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
                print(f"台架错误：{exc}")
        elif choice == "4":
            command_list(args)
            print("学生电源候选：" + ", ".join(devices.ports({(0x2E3C, 0x5740)})))
            print("CH340 候选：" + ", ".join(devices.ports({(0x34B7, 0x6877), (0x1A86, 0x7523)})))
        elif choice == "5":
            directory = input("数据目录：").strip()
            if directory:
                command_report(argparse.Namespace(directory=directory))
        else:
            print("无效选项")


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command")

    sub.add_parser("list", help="enumerate serial ports").set_defaults(func=command_list)
    discover = sub.add_parser("discover", help="probe the board")
    discover.add_argument("--sn")
    discover.set_defaults(func=command_discover)

    run = sub.add_parser("run", help="execute the case library")
    run.add_argument("--mode", choices=list(MODES))
    run.add_argument("--all", action="store_true", help="显式运行全量工况")
    run.add_argument("--case", help="仅运行指定工况名")
    run.add_argument("--buses", default="24", help="母线电压，如 24,18,12")
    run.add_argument("--groups", default="0,1,2,3,4,5,6,7")
    run.add_argument("--repeat", type=int, default=1)
    run.add_argument("--iq-limit", type=float, default=DEFAULT_IQ_LIMIT, dest="iq_limit")
    run.add_argument("--rpm-limit", type=float, default=DEFAULT_RPM_LIMIT, dest="rpm_limit")
    run.add_argument("--pos-limit", type=float, default=DEFAULT_POS_LIMIT, dest="pos_limit")
    run.add_argument("--out", default=bench.DATA_ROOT)
    run.add_argument("--sn")
    run.add_argument("--stlink-sn")
    run.add_argument("--uart", help="CH340 监测串口，电机工况必须在线")
    run.add_argument("--psu", help="学生电源串口；off 表示不连接")
    run.add_argument("--include-long", action="store_true", help="加入 60 圈位置专项")
    run.add_argument("--load-name", default="空载")
    run.add_argument("--load-mass-g", type=float)
    run.add_argument("--load-stl")
    run.add_argument("--load-axis")
    run.add_argument("--dry-run", action="store_true", dest="dry_run")
    run.add_argument("--yes", action="store_true")
    run.set_defaults(func=command_run)

    report = sub.add_parser("report", help="build REPORT.md for a run directory")
    report.add_argument("directory")
    report.set_defaults(func=command_report)

    chain = sub.add_parser("chain", help="merge a case's groups into one CSV")
    chain.add_argument("directory")
    chain.add_argument("case")
    chain.set_defaults(func=command_chain)

    args = parser.parse_args()
    if args.command is None:
        return menu(argparse.Namespace(all=False, mode=None, case=None, buses="24", groups="0,1,2,3,4,5,6,7",
            repeat=1, iq_limit=DEFAULT_IQ_LIMIT, rpm_limit=DEFAULT_RPM_LIMIT,
            pos_limit=DEFAULT_POS_LIMIT, out=bench.DATA_ROOT, sn=None, stlink_sn=None,
            uart=None, psu=None, include_long=False, dry_run=False, yes=False,
            load_name="空载", load_mass_g=None, load_stl=None, load_axis=None))
    try:
        return args.func(args)
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(f"台架错误：{exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
