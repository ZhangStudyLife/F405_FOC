"""Idle USB throughput check; leaves the student supply output off."""
import argparse
import json
import os
import time

import benchlib as bench
import devices


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--groups", default="0,1,2,3,4,5,6,7")
    parser.add_argument("--seconds", type=float, default=120)
    parser.add_argument("--psu", default="COM16")
    parser.add_argument("--sn", default=None)
    args = parser.parse_args()
    root = os.path.join(bench.DATA_ROOT, "usb_endurance_" + time.strftime("%Y%m%d_%H%M%S"))
    os.makedirs(root)
    power = devices.StudentPower(args.psu)
    link = None
    try:
        power.output(False)
        port = bench.find_port(args.sn)
        link = bench.Link(port.device, port.serial_number)
        for group in (int(x) for x in args.groups.split(",")):
            stage = os.path.join(root, f"g{group}_raw")
            os.makedirs(stage)
            raw = os.path.join(stage, "frames.f32")
            error = None
            started = time.monotonic()
            try:
                link.open_session()
                link.send("stop")
                link.send(f"send {group}")
                link.drain(.2)
                link.start_capture(raw)
                started = time.monotonic()
                until = started + args.seconds
                while time.monotonic() < until:
                    time.sleep(.1)
                    link.health()
            except Exception as exc:
                error = str(exc)
            finally:
                link.stop_capture()
                link.close_session()
            meta = {"schema": 2, "case": "usb_endurance_idle", "mode": "torque",
                    "group": group, "columns": bench.COLUMNS[group],
                    "duration_s": time.monotonic() - started,
                    "raw_bytes": os.path.getsize(raw), "error": error}
            archive = os.path.join(root, f"g{group}.7z")
            bench.archive(raw, archive, meta)
            table, check = bench.load(archive)
            with open(os.path.join(root, f"g{group}_result.json"), "w", encoding="utf-8") as file:
                json.dump(check, file, ensure_ascii=False, indent=2)
            print(f"组 {group}: {meta['duration_s']:.2f} 秒，{len(table)} 帧，"
                  f"连续性 {check['continuity']}，尾部 {check['dropped_tail_bytes']} 字节，错误 {error}", flush=True)
            if error or len(table) < args.seconds * bench.SAMPLE_HZ * .999 or any((check["continuity"]["gaps"], check["continuity"]["faults"],
                             check["continuity"]["group_mismatch"],
                             check["continuity"]["timestamp_jitter"])):
                return 1
        return 0
    finally:
        if link:
            link.send("stop")
            link.close()
        power.output(False)
        power.close()


if __name__ == "__main__":
    raise SystemExit(main())
