"""Idle USB throughput check; leaves the student supply output off."""
import argparse
import json
import os
import time

import benchlib as bench
import devices


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=120)
    parser.add_argument("--psu", required=True, help="已识别的学生电源串口")
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
        stage = os.path.join(root, "raw")
        os.makedirs(stage)
        raw = os.path.join(stage, "frames.f32")
        error = None
        link.open_session()
        link.send("stop")
        link.drain(.2)
        link.start_capture(raw)
        started = time.monotonic()
        try:
            while time.monotonic() - started < args.seconds:
                time.sleep(.1)
                link.health()
        except Exception as exc:
            error = str(exc)
        finally:
            link.stop_capture()
            link.close_session()
        meta = {"schema": 3, "case": "usb_endurance_idle", "mode": "torque",
                "columns": bench.COLUMNS, "duration_s": time.monotonic() - started,
                "raw_bytes": os.path.getsize(raw), "error": error}
        archive = os.path.join(root, "capture.7z")
        bench.archive(raw, archive, meta)
        table, check = bench.load(archive)
        with open(os.path.join(root, "result.json"), "w", encoding="utf-8") as file:
            json.dump(check, file, ensure_ascii=False, indent=2)
        print(f"{meta['duration_s']:.2f} 秒，{len(table)} 帧，连续性 {check['continuity']}，"
              f"尾部 {check['dropped_tail_bytes']} 字节，错误 {error}", flush=True)
        return int(bool(error or len(table) < args.seconds * bench.SAMPLE_HZ * .999 or any((
            check["continuity"]["gaps"], check["continuity"]["faults"],
            check["continuity"]["header_errors"], check["continuity"]["timestamp_jitter"]))))
    finally:
        if link:
            link.send("stop")
            link.close()
        power.output(False)
        power.close()


if __name__ == "__main__":
    raise SystemExit(main())
