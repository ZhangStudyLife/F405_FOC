"""Capture the ADC bring-up firmware over ST-Link without halting the CPU."""
import argparse
import csv
import json
import re
import statistics
import time
from pathlib import Path

from scope_lib import ScopeReader, find_symbol


def words(reader, address, count):
    out = reader.tel.cmd(f"mdw 0x{address:08x} {count}")
    result = []
    for line in out.splitlines():
        match = re.match(r"^0x[0-9a-fA-F]+:\s+(.*)", line.strip().lstrip("\x00"))
        if match:
            result.extend(int(x, 16) for x in match[1].split())
    if len(result) != count:
        raise RuntimeError(out)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", default="build/Debug/405_FOC.elf")
    parser.add_argument("--out", default="validation/adc-capture")
    args = parser.parse_args()
    dest = Path(args.out)
    dest.mkdir(parents=True, exist_ok=True)
    reader = ScopeReader(elf=args.elf, symbol="s_adc_scope")
    address = find_symbol(args.elf, "g_adc_test")
    if address is None:
        raise RuntimeError("ELF does not contain g_adc_test")
    try:
        reader.start()
        reader.set_frozen(False)
        before = words(reader, address, 10)
        start = time.monotonic()
        time.sleep(2)
        after = words(reader, address, 10)
        duration = time.monotonic() - start
        reader.set_frozen(True)
        time.sleep(0.02)  # allow any in-flight scope_push to finish
        info, samples = reader.read()
        tim = words(reader, 0x40010400, 18)
        gpio = {name: words(reader, addr, 6) for name, addr in
                (("A", 0x40020000), ("B", 0x40020400), ("C", 0x40020800))}
        adc = {name: words(reader, addr, 20) for name, addr in
               (("ADC1", 0x40012000), ("ADC2", 0x40012100))}
        stats = {}
        for index, name in enumerate(("PC3", "PC2", "PA4", "PA6")):
            values = [row[index] for row in samples]
            stats[name] = dict(mean=statistics.mean(values),
                               sigma=statistics.pstdev(values),
                               minimum=min(values), maximum=max(values))
        periods = [row[4] for row in samples]
        rate = 168000000 / statistics.mean(periods)
        wall_rate = ((after[1] - before[1]) & 0xffffffff) / duration
        pins_low = all((gpio[port][0] >> (pin * 2)) & 3 == 1 and
                       gpio[port][1] & (1 << pin) == 0 and
                       gpio[port][4] & (1 << pin) == 0 and
                       gpio[port][5] & (1 << pin) == 0
                       for port, pin in (("A", 7), ("B", 0), ("B", 1),
                                         ("C", 6), ("C", 7), ("C", 8)))
        checks = dict(started=after[0] == 1, pairs_complete=after[9] == 0,
                      rate_20khz=abs(rate - 20000) < 20,
                      live_count=19000 < wall_rate < 21000,
                      lifetime_periods=8200 <= after[6] <= after[7] <= 8600,
                      no_missing_periods=all(8200 <= x <= 8600 for x in periods),
                      power_pins_low=pins_low, only_ch4_enabled=tim[8] == 0x1000,
                      full_capture=len(samples) == 1024)
        report = dict(passed=all(checks.values()), checks=checks, samples=len(samples),
                      rate_hz=rate, wall_rate_hz=wall_rate, raw=stats,
                      period_cycles=[min(periods), max(periods)],
                      diagnostics=after, tim8=tim, gpio=gpio, adc=adc,
                      note="DWT rate assumes 168 MHz; GPIO register checks are not oscilloscope measurements.")
        with (dest / "samples.csv").open("w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(("PC3", "PC2", "PA4", "PA6", "period_cycles", "tim8_cnt"))
            writer.writerows(samples)
        (dest / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(json.dumps(report, indent=2))
        if not report["passed"]:
            raise SystemExit(1)
    finally:
        if reader.tel is not None:
            reader.set_frozen(False)
        reader.stop()


if __name__ == "__main__":
    main()
