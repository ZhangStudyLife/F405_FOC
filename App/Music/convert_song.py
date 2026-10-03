"""MP3/audio -> V1 motor song .inc (requires FFmpeg and NumPy)."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess

import numpy as np

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path)
parser.add_argument("name", help="C identifier and UART song name, e.g. birthdaySong")
args = parser.parse_args()
if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", args.name):
    parser.error("name must be a C identifier")

decoded = subprocess.run([
    "ffmpeg", "-v", "error", "-i", str(args.source),
    "-ac", "1", "-ar", "10000", "-f", "f32le", "pipe:1",
], check=True, stdout=subprocess.PIPE).stdout
pcm = np.frombuffer(decoded, dtype="<f4").astype(np.float64)
pcm -= pcm.mean()
pcm[:200] *= np.arange(200) / 200
pcm[-200:] *= np.arange(199, -1, -1) / 200
# Reserve interpolation headroom, using the actual half-sample peaks.
padded = np.pad(pcm, (1, 2))
mid = (-padded[:-3] + 9 * (padded[1:-2] + padded[2:-1]) - padded[3:]) / 16
peak = max(np.max(np.abs(pcm)), np.max(np.abs(mid)))
quantized = np.rint(pcm * (2000 / peak)).astype(np.int16)
padded = np.pad(quantized, (2, 4 + len(pcm) % 2)).astype(np.uint16) & 0xfff
packed = np.empty(len(padded) * 3 // 2, dtype=np.uint8)
packed[0::3] = padded[0::2] & 0xff
packed[1::3] = (padded[0::2] >> 8) | ((padded[1::2] & 15) << 4)
packed[2::3] = padded[1::2] >> 4

destination = Path(__file__).with_name(args.name + ".inc")
with destination.open("w", encoding="utf-8", newline="\n") as output:
    output.write(f"/* Motor song V1: mono 10000 Hz, signed PCM12, low sample first.\n"
                 f" * Source: {args.source.name}\n"
                 f" * SHA256: {hashlib.sha256(args.source.read_bytes()).hexdigest()}\n"
                 f" * {len(pcm)} samples, {len(pcm) / 10000:.4f} s; 20 ms edge fades.\n"
                 f" * Two leading/at least four trailing zeros; nominal peak 2000/2048. */\n"
                 f"static const uint8_t {args.name}_data[] = {{\n")
    for i in range(0, len(packed), 24):
        output.write("    " + ", ".join(f"0x{v:02x}" for v in packed[i:i + 24]) + ",\n")
    output.write(f"}};\nstatic const music_song_t {args.name} = "
                 f'{{"{args.name}", {args.name}_data, {len(pcm)}u}};\n')
print(f"{destination}: {len(pcm) / 10000:.4f} s, {len(packed)} Flash bytes")
