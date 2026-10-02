"""20-channel bench capture, raw-angle cogging table and unfiltered metrics.

Run from this workspace: python tools/speed_control.py cog|accept|fit CSV|report CSV
"""
import csv
import hashlib
import json
import sys
import time
from pathlib import Path

import numpy as np
import serial
from serial.tools import list_ports
from scipy.signal import savgol_filter

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / "auto_rpm"))
import auto_rpm as protocol


def load(path):
    rows = list(csv.DictReader(Path(path).open(encoding="utf-8-sig")))
    return {k: np.array([r[k] for r in rows], dtype=str if k == "phase" else float)
            for k in rows[0]}


def report(path):
    d = load(path)
    result = {}
    for phase in dict.fromkeys(d["phase"]):
        indices = np.flatnonzero((d["phase"] == phase) & (d["state"] == 4) & (d["fault"] == 0))
        if phase == "stop" or not len(indices):
            continue
        indices = indices[d["host_rx_s"][indices] >= d["host_rx_s"][indices[0]] + 2]
        if not len(indices):
            continue
        target, speed = d["iq_target_A"][indices], d["rpm"][indices]
        angle = np.unwrap(np.deg2rad(d["encoder_deg"][indices]))
        # Same raw-angle derivative for every version; never filter Iq target.
        raw_speed = savgol_filter(np.rad2deg(angle), 25, 3, deriv=1, delta=.002)/6
        elapsed = d["host_rx_s"][indices[-1]] - d["host_rx_s"][indices[0]]
        result[phase] = dict(frames=len(indices), iq_target_std_A=float(target.std()),
            speed_std_rpm=float(speed.std()), speed_error_rpm=float((speed-d["rpm_target"][indices]).mean()),
            raw_angle_mean_rpm=float((angle[-1]-angle[0]) / (2*np.pi) * 60 / elapsed),
            raw_angle_speed_std_rpm=float(raw_speed[25:-25].std()),
            id_rms_A=float(np.sqrt(np.mean(d["id_A"][indices]**2))),
            iq_rms_A=float(np.sqrt(np.mean(d["iq_A"][indices]**2))),
            opposing_current_fraction=float(np.mean(target*speed < 0)))
    Path(path).with_suffix(".metrics.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))


def fit(path):
    d = load(path)
    maps, validation_samples = [], []
    for direction in (1, -1):
        keep = (d["phase"] == f"cog_{direction}") & (d["state"] == 4) & (d["fault"] == 0)
        angle, current = d["encoder_deg"][keep], d["iq_target_A"][keep]
        travelled = direction * np.rad2deg(np.unwrap(np.deg2rad(angle)))
        travelled -= travelled[0]
        # Ignore the entry; each following 360 degrees is a full independent turn.
        travelled -= 6
        turns = np.floor(travelled/360).astype(int)
        assert travelled[-1] >= 1440, "Need four complete turns after entry in both directions"
        bins = (angle*(512/360)).astype(int) & 511
        per_turn = []
        for turn in range(4):
            mask = turns == turn
            counts = np.bincount(bins[mask], minlength=512)
            assert np.all(counts), "Missing angular bins"
            wave = np.bincount(bins[mask], weights=current[mask], minlength=512)/counts
            per_turn.append(wave-wave.mean())
            if turn == 3:
                validation_samples.append((angle[mask], current[mask]-current[mask].mean()))
        maps.append(np.array(per_turn))
    maps = np.array(maps)  # Calibration direction is +1 on this fitted board.
    spectra = np.fft.rfft(maps[:, :3], axis=-1)
    average = spectra.mean(axis=(0, 1))
    # Keep only angular orders stronger than their cross-turn/direction variation.
    repeatable = abs(average)**2 > np.mean(abs(spectra-average)**2, axis=(0, 1))
    repeatable[0] = False
    table = np.fft.irfft(average*repeatable, n=512)
    validation = maps[:, 3]
    before = np.sqrt(np.mean(validation**2, axis=1))
    after = np.sqrt(np.mean((validation-table)**2, axis=1))
    # LUT samples represent bin centres: shift by half a bin to firmware's angle grid.
    table = (table + np.roll(table, 1))*.5
    lookup_after = []
    for angle, current in validation_samples:
        x = angle*(512/360)
        i = x.astype(int) & 511
        lookup = table[i] + (x-i)*(table[(i+1)&511]-table[i])
        lookup_after.append(float(np.sqrt(np.mean((current-lookup)**2))))
    evidence = dict(source=str(Path(path).resolve()),
                    source_sha256=hashlib.sha256(Path(path).read_bytes()).hexdigest(),
                    retained_orders=np.flatnonzero(repeatable).tolist(),
                    validation_before_A=before.tolist(), validation_after_A=after.tolist(),
                    interpolated_lookup_validation_A=lookup_after,
                    peak_A=float(abs(table).max()))
    Path(path).with_suffix(".table.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    assert np.all(after < before), "Table did not improve both held-out turns"
    assert np.all(np.array(lookup_after) < before), "Interpolated table failed validation"
    destination = ROOT / "App/Control/cogging_table.inc"
    destination.write_text("/* " + Path(path).name + ": first three turns; fourth held out. */\n" +
        "\n".join(", ".join(f"{v:.9f}f" for v in table[i:i+8])+"," for i in range(0,512,8))+"\n", encoding="utf-8")
    print(json.dumps(evidence, indent=2))


def capture(profile):
    matches = [p for p in list_ports.comports()
               if p.serial_number == "CB832D7DC8A1944B8A2CA4E5A51DD838"]
    assert len(matches) == 1, "Fitted board UART identity not unique"
    out = ROOT / "build/bench_debug" / time.strftime("%Y%m%d_%H%M%S")
    out.mkdir()
    started = time.perf_counter()
    events = []
    with serial.Serial(matches[0].device, protocol.BAUD, timeout=.025, write_timeout=1) as port, \
            (out/(profile+".uart.bin")).open("wb") as raw_file, \
            (out/(profile+".csv")).open("w", newline="", encoding="utf-8") as file:
        original_read = port.read
        def read(count):
            chunk = original_read(count)
            raw_file.write(chunk)
            return chunk
        port.read = read
        cap = protocol.Capture(port, file)
        cap.start()
        final, error = None, None
        try:
            cap.wait(lambda d:d["zero_complete"], 3, allow_fault=True)
            protocol.stop_motor(cap)
            cap.health()
            cap.wait(lambda d:d["state"] == 0 and d["zero_ready"] and d["calibrated"] and abs(d["rpm"]) < 5, 30)
            if profile == "cog":
                phases = [(f"cog_{r}", r, 252) for r in (1,-1)]
            else:
                phases = [(f"hold_{r}_{repeat}", r, 13) for repeat in range(3)
                          for r in (1000,3000,5000,-1000,-3000,-5000)]
                phases += [(f"steady_low_{r}", r, 65 if abs(r)==1 else 15) for r in (1,-1,5,-5,50,-50)]
            for name, rpm, duration in phases:
                cap.phase = name
                events.append(dict(phase=name, host_s=time.perf_counter()-cap.origin))
                protocol.command(cap, f"rpm {rpm}")
                deadline = time.perf_counter()+duration
                next_command = time.perf_counter()
                while time.perf_counter()<deadline:
                    cap.health()
                    assert time.perf_counter()-cap.latest[0] < .5, "Stale telemetry"
                    assert time.perf_counter()-started < 3300, "Reserve five minutes for stop"
                    protocol.send(port, f"rpm {rpm}")
                    next_command = max(next_command+.01, time.perf_counter())
                    time.sleep(max(0, next_command-time.perf_counter()))
                protocol.stop_motor(cap)
                cap.wait(lambda d:abs(d["rpm"]) < 5 and not d["power_on"], 30)
            if profile == "accept":
                protocol.run_profile(cap)
        except BaseException as exc:
            error = repr(exc)
            raise
        finally:
            try:
                protocol.stop_motor(cap)
                final = cap.wait(lambda d:not d["power_on"], 3, allow_fault=True)
            finally:
                cap.close()
                (out/"result.json").write_text(json.dumps(dict(events=events, final=final, error=error,
                    frames=cap.frames, saved=cap.saved, discarded_bytes=cap.discarded,
                    elapsed_s=time.perf_counter()-started), indent=2), encoding="utf-8")
    report(out/(profile+".csv"))
    print(out)


if __name__ == "__main__":
    action = sys.argv[1]
    if action in ("cog", "accept"):
        capture(action)
    elif action == "fit":
        fit(sys.argv[2])
    elif action == "report":
        report(sys.argv[2])
