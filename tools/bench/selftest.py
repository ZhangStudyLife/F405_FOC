"""Parser self-test: no hardware, no firmware. Run before trusting a capture.

    python tools/bench/selftest.py
"""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchlib as bench  # noqa: E402


def encode(t_us, seq, payload):
    """Build one wire frame exactly as App/Control/app.c does."""
    assert len(payload) == bench.CHANNELS - 2
    word0 = (t_us & 0xFFFFFF) | ((4 << 0) << 24)  # RUN state, no fault, gates on
    word1 = seq & 0xFFFFFF
    floats = [struct.unpack("<f", struct.pack("<I", word0))[0],
              struct.unpack("<f", struct.pack("<I", word1))[0]] + list(payload)
    return struct.pack(f"<{bench.CHANNELS}f", *floats) + bench.TERMINATOR


def main():
    # 1. Byte-exact round trip of one frame through the parser.
    frame = encode(1234, 100, [float(n) for n in range(bench.CHANNELS - 2)])
    assert len(frame) == bench.FRAME_BYTES, len(frame)
    reader = bench.FrameReader()
    frames = reader.feed(frame)
    assert len(frames) == 1
    parsed = frames[0]
    assert parsed.time_us == 1234 and parsed.seq == 100
    assert parsed.state == 4 and parsed.fault == 0
    assert [parsed.channel(2 + n) for n in range(bench.CHANNELS - 2)] == [float(n) for n in range(bench.CHANNELS - 2)]

    # 2. Split and coalesced reads must give the same frames.
    reader = bench.FrameReader()
    got = []
    for cut in range(1, len(frame)):
        got += reader.feed(frame[cut - 1:cut])
    got += reader.feed(frame[-1:])
    assert len(got) == 1 and got[0].time_us == 1234
    reader = bench.FrameReader()
    got = reader.feed(frame * 5)
    assert len(got) == 5 and [f.seq for f in got] == [100] * 5

    # 3. A leading partial frame is resynchronised, not misparsed.
    reader = bench.FrameReader()
    got = reader.feed(b"\x11\x22\x33" + frame)
    assert len(got) == 1 and got[0].time_us == 1234 and reader.skipped == 3

    # 4. File parsing and continuity on a clean 2 kHz run.
    seconds = 1.0
    count = int(bench.SAMPLE_HZ * seconds)
    blob = b"".join(encode((n * 500) & 0xFFFFFF, (n * 10) & 0xFFFFFF,
                           [float(n)] * (bench.CHANNELS - 2)) for n in range(count))
    directory = tempfile.mkdtemp(prefix="foc_selftest_")
    path = os.path.join(directory, "run.f32")
    with open(path, "wb") as handle:
        handle.write(blob)
    table, dropped, words = bench.parse_file(path)
    assert table.shape == (count, bench.CHANNEL_COUNT), table.shape
    assert dropped == 0
    flags = bench.continuity(words)
    assert flags["gaps"] == 0 and flags["missing"] == 0 and flags["seq_ok"] and flags["dt_ok"]
    assert flags["header_errors"] == 0 and flags["faults"] == 0
    assert float(table[10, 2]) == 10.0  # payload slot 0 of sample 10

    # 5. A dropped sample is reported by both the timestamp and the counter.
    #    A real drop shortens the byte stream; it never shifts later frames.
    with open(path, "wb") as handle:
        for n in range(count):
            if n != 200:
                handle.write(encode((n * 500) & 0xFFFFFF, (n * 10) & 0xFFFFFF,
                                    [float(n)] * (bench.CHANNELS - 2)))
    table, _, words = bench.parse_file(path)
    assert table.shape[0] == count - 1
    flags = bench.continuity(words)
    assert flags["gaps"] == 1 and flags["missing"] == 1 and not flags["seq_ok"]
    assert flags["max_gap_us"] == 1000

    # 6. A torn tail is counted, and a byte loss inside the stream is resynced.
    with open(path, "wb") as handle:
        handle.write(blob + b"\x11" * bench.FRAME_BYTES)
    table, dropped, words = bench.parse_file(path)
    assert len(table) == count and dropped == bench.FRAME_BYTES
    assert bench.continuity(words)["gaps"] == 0
    with open(path, "wb") as handle:
        handle.write(blob[:200 * bench.FRAME_BYTES + bench.FRAME_BYTES - 4] + blob[201 * bench.FRAME_BYTES + 8:])
    table, dropped, words = bench.parse_file(path)
    assert len(table) == count - 2 and dropped == 2 * bench.FRAME_BYTES - 12
    flags = bench.continuity(words)
    assert flags["gaps"] == 1 and flags["missing"] == 2

    # 6b. A torn head is resynchronised rather than shifting every channel.
    with open(path, "wb") as handle:
        handle.write(b"\x11\x22\x33" + blob)
    table, dropped, words = bench.parse_file(path)
    assert dropped == 3 + (len(blob) % bench.FRAME_BYTES)
    assert float(table[10, 2]) == 10.0
    assert bench.continuity(words)["gaps"] == 0

    # 7. Channel map and archive round trip.
    from benchlib import CHANNELS, COLUMNS, CHANNEL_NAMES
    assert CHANNELS == 15 and COLUMNS[2:] == CHANNEL_NAMES
    assert len(COLUMNS) == bench.CHANNEL_COUNT == 15
    with open(path, "wb") as handle:
        handle.write(blob)
    meta = {"raw_bytes": len(blob), "frames": count, "case": "selftest"}
    target = bench.archive(path, os.path.join(directory, "case.zip"), meta)
    assert not os.path.exists(path)
    loaded, loaded_meta = bench.load(target)
    assert loaded.shape == (count, bench.CHANNEL_COUNT)
    assert loaded_meta["parsed_frames"] == count
    assert loaded_meta["continuity"]["gaps"] == 0
    stats = bench.summarise(loaded, loaded_meta)
    assert stats["frames"] == count and stats["faults"] == 0
    stage = os.path.join(directory, "new_segment")
    os.mkdir(stage)
    new_raw = os.path.join(stage, "frames.f32")
    with open(new_raw, "wb") as handle:
        handle.write(blob)
    new_meta = dict(meta, columns=bench.COLUMNS, schema=3)
    new_target = bench.archive(new_raw, os.path.join(directory, "case.7z"), new_meta)
    assert not os.path.exists(stage)
    new_table, new_loaded = bench.load(new_target)
    assert new_table.shape == loaded.shape and new_loaded["continuity"]["gaps"] == 0

    capture = os.path.join(directory, "capture.foc3")
    sample = (17, 2048, 2047, 1040, 4100, 1000, 2000, 3000, 0x84,
              12.5, 87.5, 0.1, -0.2, 0.3, 1.2, -0.4)
    with open(capture, "wb") as handle:
        handle.write(bench.FOC3_HEADER.pack(0x33434F46, 2))
        handle.write(bench.FOC3_RECORD.pack(*sample))
        handle.write(bench.FOC3_RECORD.pack((sample[0] + 1) & 0xFFFFFF, *sample[1:]))
    assert len(bench.parse_foc_capture(capture)) == 2
    with open(capture, "wb") as handle:
        handle.write(bench.FOC3_HEADER.pack(0x33434F46, 2))
        handle.write(bench.FOC3_RECORD.pack(*sample))
        handle.write(bench.FOC3_RECORD.pack(sample[0] + 2, *sample[1:]))
    try:
        bench.parse_foc_capture(capture)
        raise AssertionError("FOC3 sequence gap accepted")
    except ValueError as exc:
        assert "sequence gap" in str(exc)

    # 8. The command-plan validator refuses anything outside the envelope.
    import bench as driver
    over = driver.Experiment("over", "torque", 1.0, [(0, "Iq 1.00")], {})
    assert over.check(0.4, 1000.0, 800.0) is not None
    inside = driver.Experiment("inside", "speed", 1.0, [(0, "rpm -500")], {})
    assert inside.check(0.4, 1000.0, 800.0) is None
    empty = driver.Experiment("empty", "torque", 1.0, [], {})
    assert empty.check(0.4, 1000.0, 800.0) is not None
    assert "speed_atlas" not in {case.case for case in driver.speed_cases()}
    assert [case.case for case in driver.position_cases()] == ["position_profile_1"]

    print(f"PASS: frame round trip, split/coalesced reads, resync, {count} frame file, "
          f"gap detection, FOC3 capture, archive round trip, plan limits")
    return 0


if __name__ == "__main__":
    sys.exit(main())
