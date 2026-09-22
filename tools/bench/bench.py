"""Motor bench driver: one command line, deterministic conditions, saved data.

    bench.py list                       enumerate serial ports
    bench.py discover                   probe the board and check the frame layout
    bench.py run --mode torque|speed|position|all [--sweep]
    bench.py report <run-directory>
    bench.py chain <run-directory>      merge a case's four groups by timestamp

Every case is one file per logging group: ``out/<stamp>/<mode>/<case>_g<N>.zip``
holding ``frames.f32`` and ``meta.json``. The same case repeated on groups 0..3
runs the identical command timeline, so the four parts can be joined offline.
"""
import argparse
import json
import math
import os
import sys
import tempfile
import time
from dataclasses import dataclass, field

import numpy as np

import benchlib as bench

# Safety envelope. The board has no throttle of its own for these modes, so the
# host is the last line of defence; widen these only on purpose.
DEFAULT_IQ_LIMIT = 0.4      # A, provisional: the current gain is uncalibrated.
DEFAULT_RPM_LIMIT = 1000.0  # RPM. The motor rating is 9400; start far below.
DEFAULT_POS_LIMIT = 1200.0  # degrees of absolute target, enough for three turns.
RAMP_RATE = 20.0            # A/s of commanded reference, above the firmware's slew.
WARMUP_MS = 300             # settle before the plan starts; 300 samples of 1 ms.
KEEPALIVE_MS = 100          # must stay below the firmware's 200 ms watchdog.


@dataclass
class Experiment:
    """One repeatable operating condition."""

    case: str
    mode: str
    duration_s: float
    plan: list = field(default_factory=list)  # [(t_ms, "Iq 0.20"), ...]
    targets: dict = field(default_factory=dict)
    note: str = ""

    def check(self, iq_limit, rpm_limit, pos_limit):
        """Reject a plan that leaves the safe envelope before it is ever sent."""
        for _, command in self.plan:
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

def torque_cases(amp_steps=5, rate_steps=3, per_cel=3.0):
    """Torque: fixed loads, steps, ramps, impulses, sinusoids, chirps, sign
    reversal, and a dynamic high-speed/low-torque operating point."""
    cases = []
    amps = [round(0.05 + 0.35 * n / max(1, amp_steps - 1), 3) for n in range(amp_steps)]
    rates = [0.5, 2.0, 10.0][:rate_steps]
    for a in amps:
        for sign in (1, -1):
            tag = "pos" if sign > 0 else "neg"
            cases.append(Experiment(f"const_{tag}_{a:.2f}", "torque", 1.5 + per_cel,
                                    [(0, f"Iq {sign * a:.2f}")],
                                    {"iq": sign * a}, "static torque"))
            level = sign * amps[-1]
            cases.append(Experiment(f"step_{tag}_{a:.2f}", "torque", 0.4 + per_cel,
                                    [(0, "Iq 0.00"), (300, f"Iq {level:.2f}")],
                                    {"iq": level}, "reference step"))
            cases.append(Experiment(f"impulse_{tag}_{a:.2f}", "torque", 1.0 + per_cel,
                                    [(0, "Iq 0.00"), (300, f"Iq {level:.2f}"),
                                     (300 + max(20, int(per_cel * 200)), "Iq 0.00")],
                                    {"iq": level}, "short torque pulse"))
        for rate in rates:
            span = a / rate * 1000.0
            steps = max(2, int(span / 20.0))
            plan = [(int(n * span / steps), f"Iq {a * n / steps:.3f}") for n in range(steps + 1)]
            plan += [(int(span + 300 + n * span / steps), f"Iq {a * (1.0 - n / steps):.3f}")
                     for n in range(steps + 1)]
            cases.append(Experiment(f"ramp_{a:.2f}_{rate:g}", "torque",
                                    math.ceil((2 * span + 300) / 1000.0) + per_cel,
                                    plan, {"iq": a, "rate_a_per_s": rate}, "triangular ramp"))
    for freq in (0.2, 1.0, 2.0, 5.0, 10.0):
        duration = 1.0 + per_cel
        plan = []
        for n in range(int(duration * 1000.0 / 20.0)):
            t = n * 20.0
            value = amps[-1] * math.sin(2 * math.pi * freq * t / 1000.0)
            plan.append((int(t), f"Iq {value:.3f}"))
        cases.append(Experiment(f"sin_{freq:g}", "torque", duration, plan,
                                {"iq": amps[-1], "freq_hz": freq}, "sine torque"))
    duration = 1.0 + per_cel
    plan = []
    for n in range(int(duration * 1000.0 / 20.0)):
        t = n * 20.0 / 1000.0
        f0, f1 = 0.1, 50.0
        phase = 2 * math.pi * (f0 * t + (f1 - f0) * t * t / (2 * duration))
        plan.append((int(n * 20.0), f"Iq {amps[-1] * math.sin(phase):.3f}"))
    cases.append(Experiment("chirp_0p1_50", "torque", duration, plan,
                            {"iq": amps[-1], "f0": 0.1, "f1": 50.0}, "linear chirp"))
    period = 1000.0
    duration = 1.0 + per_cel
    plan = [(int(n * 20.0), f"Iq {amps[-1] * 0.6 * (1 if (n * 20.0 % period) < period / 2 else -1):.3f}")
            for n in range(int(duration * 1000.0 / 20.0))]
    cases.append(Experiment("sign_reverse", "torque", duration, plan,
                            {"iq": amps[-1] * 0.6, "period_ms": period}, "sign reversal"))
    return cases


def speed_cases(amp_steps=5, rate_steps=3, per_cel=3.0):
    """Speed: fixed points, steps, cross-zero, ramps, sinusoids, square, chirp."""
    cases = []
    rpms = [round(50 + 950 * n / max(1, amp_steps - 1)) for n in range(amp_steps)]
    rates = [100.0, 500.0, 2000.0][:rate_steps]
    for r in rpms:
        for sign in (1, -1):
            tag = "pos" if sign > 0 else "neg"
            cases.append(Experiment(f"const_{tag}_{r}", "speed", 0.5 + per_cel,
                                    [(0, f"rpm {sign * r}")],
                                    {"rpm": sign * r}, "constant speed"))
    level = rpms[-1]
    cases.append(Experiment("step_up", "speed", 0.5 + per_cel,
                            [(0, "rpm 0"), (300, f"rpm {level}")],
                            {"rpm": level}, "step from standstill"))
    cases.append(Experiment("step_zero_cross", "speed", 0.5 + 2 * per_cel,
                            [(0, f"rpm {level // 2}"), (300 + per_cel * 1000,
                                                        f"rpm {-level // 2}")],
                            {"rpm": f"+/-{level // 2}"}, "step across zero"))
    for rate in rates:
        span = level / rate * 1000.0
        steps = max(2, int(span / 20.0))
        plan = [(int(n * span / steps), f"rpm {level * n / steps:.1f}") for n in range(steps + 1)]
        plan += [(int(span + 300 + n * span / steps), f"rpm {level * (1.0 - n / steps):.1f}")
                 for n in range(steps + 1)]
        cases.append(Experiment(f"ramp_{rate:g}", "speed",
                                math.ceil((2 * span + 300) / 1000.0) + per_cel, plan,
                                {"rpm": level, "rate_rpm_per_s": rate}, "speed ramp"))
    for freq in (0.2, 1.0, 2.0, 5.0):
        duration = 1.0 + per_cel
        amplitude = level / 3.0
        plan = [(int(n * 20.0), f"rpm {amplitude * math.sin(2 * math.pi * freq * n * 20.0 / 1e6):.1f}")
                for n in range(int(duration * 1000.0 / 20.0))]
        cases.append(Experiment(f"sin_{freq:g}", "speed", duration, plan,
                                {"rpm": amplitude, "freq_hz": freq}, "sine speed"))
    duration = 1.0 + per_cel
    plan = [(int(n * 20.0), f"rpm {level / 3.0 * (1 if (n * 20.0 % 500.0) < 250.0 else -1):.1f}")
            for n in range(int(duration * 1000.0 / 20.0))]
    cases.append(Experiment("square", "speed", duration, plan,
                            {"rpm": level / 3.0, "period_ms": 500.0}, "square speed"))
    return cases


def position_cases(amp_steps=5, rate_steps=3, per_cel=3.0):
    """Position: absolute steps, multi-turn moves, point-to-point, trapezoids,
    sinusoids, and reversal symmetry."""
    cases = []
    spans = [90.0, 180.0, 360.0, 720.0, 1080.0]
    spans = spans[:max(2, amp_steps)] if amp_steps <= 5 else spans
    for span in spans:
        for sign in (1, -1):
            tag = "pos" if sign > 0 else "neg"
            cases.append(Experiment(f"step_{tag}_{int(span)}", "position", 1.5 + per_cel,
                                    [(0, f"pos {sign * span:.2f}")],
                                    {"pos": sign * span}, "absolute step"))
    cases.append(Experiment("p2p_360", "position", 1.0 + 3 * per_cel,
                            [(0, "pos 0.00"), (500, "pos 360.00"),
                             (500 + int(per_cel * 1000), "pos 0.00"),
                             (500 + 2 * int(per_cel * 1000), "pos 360.00")],
                            {"pos": "0..360"}, "point to point"))
    cases.append(Experiment("p2p_720", "position", 1.0 + 3 * per_cel,
                            [(0, "pos 0.00"), (500, "pos 720.00"),
                             (500 + int(per_cel * 1000), "pos 0.00"),
                             (500 + 2 * int(per_cel * 1000), "pos -720.00")],
                            {"pos": "0..+/-720"}, "multi-turn point to point"))
    sequence = [(0, "pos 0.00")]
    for n, angle in enumerate((90.0, 180.0, 270.0, 360.0, 720.0, 360.0, 0.0)):
        sequence.append((500 + n * 700, f"pos {angle:.2f}"))
    cases.append(Experiment("multi_seg", "position", 1.0 + 7 * 0.7 + per_cel, sequence,
                            {"pos": "0/90/180/270/360/720"}, "segmented multi-turn"))
    for velocity in (60.0, 180.0, 360.0)[:max(1, rate_steps)]:
        span = spans[2]
        run_ms = span / velocity * 1000.0
        plan = []
        for n in range(int(run_ms / 20.0) + 1):  # up
            plan.append((int(n * 20.0), f"pos {velocity * n * 20.0 / 1000.0:.2f}"))
        for n in range(int(run_ms / 20.0) + 1):  # back down
            plan.append((int(run_ms + n * 20.0), f"pos {span - velocity * n * 20.0 / 1000.0:.2f}"))
        cases.append(Experiment(f"trap_v{int(velocity)}", "position",
                                0.2 + (2 * run_ms) / 1000.0 + per_cel, plan,
                                {"pos": span, "velocity_deg_per_s": velocity},
                                "triangular position profile"))
    for freq in (0.5, 1.0, 2.0, 5.0):
        duration = 1.0 + per_cel
        amplitude = 180.0
        plan = [(int(n * 20.0),
                 f"pos {amplitude * math.sin(2 * math.pi * freq * n * 20.0 / 1e6):.2f}")
                for n in range(int(duration * 1000.0 / 20.0))]
        cases.append(Experiment(f"sin_{freq:g}", "position", duration, plan,
                                {"pos": amplitude, "freq_hz": freq}, "sine position"))
    cases.append(Experiment("reversal_symmetry", "position", 1.0 + 2 * per_cel,
                            [(0, "pos 0.00"), (500, "pos 360.00"),
                             (500 + int(per_cel * 1000), "pos -360.00")],
                            {"pos": "+/-360"}, "reversal symmetry"))
    return cases


MODES = {"torque": torque_cases, "speed": speed_cases, "position": position_cases}


# ------------------------------------------------------------------ execution

def run_case(link, out_dir, experiment, group, limits, repetitions=1):
    """Capture one (case, group) pair on a temporary file and zip it."""
    iq_limit, rpm_limit, pos_limit = limits
    problem = experiment.check(iq_limit, rpm_limit, pos_limit)
    if problem:
        raise SystemExit(f"SAFETY: {problem}")
    results = []
    for repeat in range(repetitions):
        raw = os.path.join(out_dir, f"{experiment.case}_g{group}_r{repeat}.f32")
        link.open_session()
        try:
            # Deterministic pre-roll: identical starting state every repeat.
            bench.wait_idle(link)
            link.send("send 3")
            link.listen(0.15)  # let the group change land
            if experiment.mode == "position":
                link.send("zero")  # the origin is the only stateful input
                link.listen(0.1)
            link.send(f"send {group}")
            link.listen(0.15)
            link.start_capture(raw)
            time.sleep(WARMUP_MS / 1000.0)
            scheduler = bench.Scheduler(link, keepalive_ms=KEEPALIVE_MS)
            scheduler.run(experiment.plan, experiment.duration_s)
            link.send("stop")
            time.sleep(0.2)  # let the coast-down tail into the file
        finally:
            path = link.stop_capture()
            link.close_session()
        meta = {
            "case": experiment.case,
            "mode": experiment.mode,
            "group": group,
            "group_name": bench.GROUP_NAMES[group],
            "repeat": repeat,
            "note": experiment.note,
            "targets": experiment.targets,
            "duration_s": experiment.duration_s,
            "warmup_ms": WARMUP_MS,
            "keepalive_ms": KEEPALIVE_MS,
            "ramp_rate_a_per_s": RAMP_RATE,
            "plan": [[int(t), c] for t, c in experiment.plan],
            "command_jitter_ms": scheduler.jitter,
            "raw_bytes": os.path.getsize(path),
            "limits": {"iq": iq_limit, "rpm": rpm_limit, "pos": pos_limit},
            "columns": bench.COLUMNS[group],
            "archive": os.path.basename(path).replace(".f32", ".zip"),
            "host": time.strftime("%Y-%m-%d %H:%M:%S"),
            "git": git_revision(),
        }
        target = os.path.join(out_dir, meta["archive"])
        bench.archive(path, target, meta)
        # Continuity is measured from the archive so a torn capture is caught now.
        table, loaded = bench.load(target)
        loaded["continuity"]["path"] = target
        results.append(loaded)
        flags = loaded["continuity"]
        if flags.get("faults"):
            raise SystemExit(f"FAULT during {experiment.case} g{group}: "
                             f"{flags['faults']} frames flagged; stopping the run")
        if not flags.get("dt_ok", False):
            raise SystemExit(f"SAFETY: {experiment.case} g{group} lost 20 kHz continuity "
                             f"({flags.get('gaps')} gaps, max {flags.get('max_gap_us')} us); "
                             f"stopping the run")
        print(f"  {experiment.case:<22} g{group}  r{repeat}  "
              f"{table.shape[0]:>9} frames  {loaded['raw_bytes'] / 1e6:6.2f} MB  "
              f"{os.path.getsize(target) / 1e6:6.2f} MB zip")
    return results


def git_revision():
    try:
        import subprocess
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        out = subprocess.run(["git", "-C", root, "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=5)
        return out.stdout.strip() or None
    except Exception:
        return None


def command_run(args):
    import benchlib as lib
    modes = list(MODES) if args.mode == "all" else [args.mode]
    limits = (args.iq_limit, args.rpm_limit, args.pos_limit)
    cases = []
    for mode in modes:
        for experiment in MODES[mode](args.amp_steps, args.rate_steps, args.per_cel):
            problem = experiment.check(*limits)
            if problem:
                print(f"SKIP {problem}")
                continue
            cases.append(experiment)
    groups = [int(g) for g in args.groups.split(",")]
    total = sum(experiment.duration_s + 1.2 for experiment in cases) * len(groups) * args.repeat
    print(f"{len(cases)} cases x {len(groups)} groups x {args.repeat} repeats "
          f"~ {total / 60:.1f} min")
    if args.dry_run:
        for experiment in cases:
            print(f"  {experiment.mode:<8} {experiment.case:<22} {experiment.duration_s:5.1f}s "
                  f"{len(experiment.plan):>5} commands  {experiment.note}")
        return 0
    if not args.yes:
        if input("start? [y/N] ").strip().lower() != "y":
            return 1
    stamp = time.strftime("%Y%m%d_%H%M%S")
    root = os.path.abspath(args.out)
    out_dir = os.path.join(root, stamp)
    os.makedirs(out_dir, exist_ok=True)
    session = {
        "started": stamp, "modes": modes, "groups": groups, "repeat": args.repeat,
        "limits": {"iq": args.iq_limit, "rpm": args.rpm_limit, "pos": args.pos_limit},
        "git": git_revision(), "cases": len(cases), "directory": out_dir,
    }
    with open(os.path.join(out_dir, "session.json"), "w", encoding="utf-8") as handle:
        json.dump(session, handle, indent=2, ensure_ascii=False)

    port = bench.find_port(args.sn)
    print(f"port {port.device} sn={port.serial_number}")
    link = bench.Link(port.device, port.serial_number)
    completed = 0
    try:
        link.open_session()
        link.send("stop")
        link.listen(0.2)
        for experiment in cases:
            print(f"{experiment.mode}/{experiment.case} ({experiment.note})")
            for group in groups:
                run_case(link, out_dir, experiment, group, limits, args.repeat)
            completed += 1
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        try:
            link.send("stop")
            time.sleep(0.2)
        finally:
            link.close()
    session["completed_cases"] = completed
    session["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
    with open(os.path.join(out_dir, "session.json"), "w", encoding="utf-8") as handle:
        json.dump(session, handle, indent=2, ensure_ascii=False)
    print(f"{completed}/{len(cases)} cases in {out_dir}")
    return 0


def command_report(args):
    files = sorted(f for f in os.listdir(args.directory) if f.endswith(".zip"))
    if not files:
        raise SystemExit(f"no captures in {args.directory}")
    lines = [f"# 台架数据报告", "",
             f"- 目录：`{os.path.abspath(args.directory)}`",
             f"- 文件：{len(files)}", ""]
    rows = []
    for name in files:
        table, meta = bench.load(os.path.join(args.directory, name))
        stats = bench.summarise(table, meta)
        flags = meta.get("continuity", {})
        rows.append((meta["case"], meta["group"], stats, flags))
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
    target = os.path.join(args.directory, "REPORT.md")
    with open(target, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    print(f"wrote {target}")
    return 0


def command_chain(args):
    """Join one case's four groups on the sample counter into a wide table."""
    files = sorted(f for f in os.listdir(args.directory)
                   if f.startswith(args.case) and f.endswith(".zip"))
    if len(files) < 2:
        raise SystemExit(f"need the same case on several groups, found {files}")
    merged = None
    columns = []
    for name in files:
        table, meta = bench.load(os.path.join(args.directory, name))
        words = np.frombuffer(table[:, :12].tobytes(), dtype="<u4").reshape(-1, 12)
        seq = (words[:, 1] & 0xFFFFFF).astype(np.int64)
        group = int(words[0, 1] >> 24)
        names = [f"g{group}_{c}" for c in bench.GROUP_CHANNELS[group]]
        block = table[:, 2:12].astype(np.float64)
        if merged is None:
            merged = {"seq": seq, "t_us": (words[:, 0] & 0xFFFFFF).astype(np.int64)}
            columns = ["seq", "t_us"]
        else:
            if not np.array_equal(seq, merged["seq"]):
                raise SystemExit(f"{name}: sample counters do not line up with the first group")
        for index, label in enumerate(names):
            merged[label] = block[:, index]
            columns.append(label)
    matrix = np.column_stack([merged[c] for c in columns])
    stem = os.path.join(args.directory, f"{args.case}_merged")
    np.savetxt(stem + ".csv", matrix, delimiter=",", header=",".join(columns),
               comments="", fmt="%.6g")
    print(f"wrote {stem}.csv  ({matrix.shape[0]} rows x {matrix.shape[1]} columns)")
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


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="enumerate serial ports").set_defaults(func=command_list)
    discover = sub.add_parser("discover", help="probe the board")
    discover.add_argument("--sn")
    discover.set_defaults(func=command_discover)

    run = sub.add_parser("run", help="execute the case library")
    run.add_argument("--mode", default="all", choices=["all", *MODES])
    run.add_argument("--groups", default="0,1,2,3")
    run.add_argument("--repeat", type=int, default=1)
    run.add_argument("--per-cel", type=float, default=3.0, dest="per_cel")
    run.add_argument("--amp-steps", type=int, default=5, dest="amp_steps")
    run.add_argument("--rate-steps", type=int, default=3, dest="rate_steps")
    run.add_argument("--iq-limit", type=float, default=DEFAULT_IQ_LIMIT, dest="iq_limit")
    run.add_argument("--rpm-limit", type=float, default=DEFAULT_RPM_LIMIT, dest="rpm_limit")
    run.add_argument("--pos-limit", type=float, default=DEFAULT_POS_LIMIT, dest="pos_limit")
    run.add_argument("--out", default=os.path.join(tempfile.gettempdir(), "foc_bench"))
    run.add_argument("--sn")
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
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
