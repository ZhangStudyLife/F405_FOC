"""Parser self-test: no hardware, no firmware. Run before trusting a capture.

    python tools/bench/selftest.py
"""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchlib as bench  # noqa: E402


def encode(t_us, seq, group, payload):
    """Build one wire frame exactly as App/Control/app.c does."""
    assert len(payload) == 10
    word0 = (t_us & 0xFFFFFF) | ((4 << 0) << 24)  # RUN state, no fault, gates on
    word1 = (seq & 0xFFFFFF) | (group << 24)
    floats = [struct.unpack("<f", struct.pack("<I", word0))[0],
              struct.unpack("<f", struct.pack("<I", word1))[0]] + list(payload)
    return struct.pack("<12f", *floats) + bench.TERMINATOR


def main():
    # 1. Byte-exact round trip of one frame through the parser.
    frame = encode(1234, 99, 2, [float(n) for n in range(10)])
    assert len(frame) == bench.FRAME_BYTES, len(frame)
    reader = bench.FrameReader()
    frames = reader.feed(frame)
    assert len(frames) == 1
    parsed = frames[0]
    assert parsed.time_us == 1234 and parsed.seq == 99 and parsed.group == 2
    assert parsed.state == 4 and parsed.fault == 0
    assert [parsed.channel(2 + n) for n in range(10)] == [float(n) for n in range(10)]

    # 2. Split and coalesced reads must give the same frames.
    reader = bench.FrameReader()
    got = []
    for cut in range(1, len(frame)):
        got += reader.feed(frame[cut - 1:cut])
    got += reader.feed(frame[-1:])
    assert len(got) == 1 and got[0].time_us == 1234
    reader = bench.FrameReader()
    got = reader.feed(frame * 5)
    assert len(got) == 5 and [f.seq for f in got] == [99] * 5

    # 3. A leading partial frame is resynchronised, not misparsed.
    reader = bench.FrameReader()
    got = reader.feed(b"\x11\x22\x33" + frame)
    assert len(got) == 1 and got[0].time_us == 1234 and reader.skipped == 3

    # 4. File parsing and continuity on a clean 20 kHz run.
    seconds = 0.1
    count = int(bench.SAMPLE_HZ * seconds)
    blob = b"".join(encode((n * 50) & 0xFFFFFF, n & 0xFFFFFF, 3,
                           [float(n)] * 10) for n in range(count))
    directory = tempfile.mkdtemp(prefix="foc_selftest_")
    path = os.path.join(directory, "run.f32")
    with open(path, "wb") as handle:
        handle.write(blob)
    table, dropped, words = bench.parse_file(path)
    assert table.shape == (count, bench.CHANNEL_COUNT), table.shape
    assert dropped == 0
    flags = bench.continuity(words, 3)
    assert flags["gaps"] == 0 and flags["missing"] == 0 and flags["seq_ok"] and flags["dt_ok"]
    assert flags["group_mismatch"] == 0 and flags["faults"] == 0
    assert float(table[10, 2]) == 10.0  # payload slot 0 of sample 10

    # 5. A dropped sample is reported by both the timestamp and the counter.
    #    A real drop shortens the byte stream; it never shifts later frames.
    with open(path, "wb") as handle:
        for n in range(count):
            if n != 200:
                handle.write(encode((n * 50) & 0xFFFFFF, n & 0xFFFFFF, 3, [float(n)] * 10))
    table, _, words = bench.parse_file(path)
    assert table.shape[0] == count - 1
    flags = bench.continuity(words, 3)
    assert flags["gaps"] == 1 and flags["missing"] == 1 and not flags["seq_ok"]
    assert flags["max_gap_us"] == 100

    # 6. A torn tail is counted, and a byte loss inside the stream is resynced.
    with open(path, "wb") as handle:
        handle.write(blob + b"\x11" * bench.FRAME_BYTES)
    table, dropped, words = bench.parse_file(path)
    assert len(table) == count and dropped == bench.FRAME_BYTES
    assert bench.continuity(words, 3)["gaps"] == 0
    with open(path, "wb") as handle:
        handle.write(blob[:200 * 52 + 48] + blob[201 * 52 + 8:])
    table, dropped, words = bench.parse_file(path)
    assert len(table) == count - 2 and dropped == 92
    flags = bench.continuity(words, 3)
    assert flags["gaps"] == 1 and flags["missing"] == 2

    # 6b. A torn head is resynchronised rather than shifting every channel.
    with open(path, "wb") as handle:
        handle.write(b"\x11\x22\x33" + blob)
    table, dropped, words = bench.parse_file(path)
    assert dropped == 3 + (len(blob) % bench.FRAME_BYTES)
    assert float(table[10, 2]) == 10.0
    assert bench.continuity(words, 3)["gaps"] == 0

    # 7. Channel map and archive round trip.
    from benchlib import CHANNELS, COLUMNS, GROUP_CHANNELS
    for group, names in GROUP_CHANNELS.items():
        assert len(names) == CHANNELS - 2, group
        assert len(COLUMNS[group]) == bench.CHANNEL_COUNT
        assert COLUMNS[group][2:12] == [f"g{group}_{n}" for n in names]
    with open(path, "wb") as handle:
        handle.write(blob)
    meta = {"group": 3, "raw_bytes": len(blob), "frames": count, "case": "selftest"}
    target = bench.archive(path, os.path.join(directory, "case_g3.zip"), meta)
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
    new_meta = dict(meta, columns=bench.COLUMNS[3], schema=2)
    new_target = bench.archive(new_raw, os.path.join(directory, "case_g3.7z"), new_meta)
    assert not os.path.exists(stage)
    new_table, new_loaded = bench.load(new_target)
    assert new_table.shape == loaded.shape and new_loaded["continuity"]["gaps"] == 0

    # 8. The command-plan validator refuses anything outside the envelope.
    import bench as driver
    over = driver.Experiment("over", "torque", 1.0, [(0, "Iq 1.00")], {})
    assert over.check(0.4, 1000.0, 800.0) is not None
    inside = driver.Experiment("inside", "speed", 1.0, [(0, "rpm -500")], {})
    assert inside.check(0.4, 1000.0, 800.0) is None
    empty = driver.Experiment("empty", "torque", 1.0, [], {})
    assert empty.check(0.4, 1000.0, 800.0) is not None

    print(f"PASS: frame round trip, split/coalesced reads, resync, {count} frame file, "
          f"gap and corruption detection, group map, archive round trip, plan limits")
    return 0


if __name__ == "__main__":
    sys.exit(main())
