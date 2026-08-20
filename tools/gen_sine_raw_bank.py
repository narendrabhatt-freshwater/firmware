#!/usr/bin/env python3
"""Write cmi_control/waves/w0..w255 sine bank as 48 kHz int16 LE .raw files.

All 256 ids share the same pure sine so heard pitch comes only from
note_Hz / root_Hz (polyphony check). Frequency is 260 Hz so a short cell
closes an integer number of cycles (seamless body loop). Named sine_c4
to match the C4-class root used on the card.

Usage:
  python3 tools/gen_sine_raw_bank.py
"""

from __future__ import annotations

import math
import struct
from pathlib import Path

SR = 48000
# Multiple of 5 Hz → 9600-sample cell = exact integer cycles (seamless loop).
ROOT_HZ = 260.0
CELL = 9600  # 200 ms @ 48 kHz
assert abs(CELL * ROOT_HZ / SR - round(CELL * ROOT_HZ / SR)) < 1e-9

# Longer than kAttackSamples (512) so LoadWave has attack + body.
# 30 s is enough to hold a note while testing; ~700 MB for 256 files.
SECONDS = 30
TOTAL = SR * SECONDS
assert TOTAL % CELL == 0

WAVES = 256
TARGET_PEAK = 10 ** (-1.0 / 20.0)  # −1 dBFS
TWO_PI = 2.0 * math.pi

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "cmi_control" / "waves"


def make_cell() -> bytes:
    cycles = int(round(CELL * ROOT_HZ / SR))
    frames = bytearray()
    for i in range(CELL):
        s = TARGET_PEAK * math.sin(TWO_PI * cycles * i / CELL)
        frames += struct.pack("<h", int(max(-1.0, min(1.0, s)) * 32767.0))
    return bytes(frames)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("w*_*.raw"):
        old.unlink()
    for old in OUT.glob("w*_*.wav"):
        old.unlink()

    cell = make_cell()
    tiles = TOTAL // CELL
    blob = cell * tiles
    root = int(round(CELL * ROOT_HZ / SR)) * SR / CELL

    for slot in range(WAVES):
        path = OUT / f"w{slot}_sine_c4.raw"
        path.write_bytes(blob)
        if slot == 0 or slot == WAVES - 1 or (slot + 1) % 32 == 0:
            print(f"{path.name}: {TOTAL} samples @ {SR} Hz int16 LE, "
                  f"sine {root:.4f} Hz, {SECONDS}s")

    (OUT / "roots.txt").write_text(
        "# wave_id root_hz loop — identical sines; pitch = note_hz/root\n"
        + "".join(f"{i} {root:.6g} loop\n" for i in range(WAVES)),
        encoding="utf-8",
    )
    print(f"wrote {WAVES} files + {OUT / 'roots.txt'} (root={root:.4f} Hz)")


if __name__ == "__main__":
    main()
