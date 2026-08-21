#!/usr/bin/env python3
"""Generate 256 identical C4 sines for Channel Card wave ids 0..255.

Every file is the same 260 Hz loop (seamless 9600-sample cell). roots.txt
sets root = 260 on every id, so heard pitch is only note_Hz / root
(MIDI C5 = 2×, C3 = ½×). A chord that still sounds like one pitch is a
routing bug, not the bank.

260 Hz (not 261.625565) so a 200 ms cell is an integer number of cycles.

Usage:
  python3 scripts/gen_wave_bank.py
"""

from __future__ import annotations

import math
import struct
from pathlib import Path

SR = 48000
NWAVES = 256
TWO_PI = 2.0 * math.pi
TARGET_PEAK = 10 ** (-1.0 / 20.0)
# 9600 samples @ 260 Hz = exactly 52 cycles. Body starts at attack-32 = 480.
CELL = 9600
ATTACK = 512
XFADE = 32
BODY_CELLS = 5
N = (ATTACK - XFADE) + CELL * BODY_CELLS  # 48480
ROOT_HZ = 260.0
OUT_DIR = Path(__file__).resolve().parents[1] / "cmi_control" / "waves"


def clamp(x: float, lo: float = -1.0, hi: float = 1.0) -> float:
    return lo if x < lo else hi if x > hi else x


def make_sine(n: int, hz: float) -> tuple[list[float], float]:
    cycles = int(round(CELL * hz / SR))
    root = cycles * SR / CELL
    out: list[float] = []
    for i in range(n):
        s = TARGET_PEAK * math.sin(TWO_PI * root * i / SR)
        if i < 48:
            a = i / 47.0
            a = a * a * (3.0 - 2.0 * a)
            s *= a
        out.append(clamp(s))
    return out, root


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for old in OUT_DIR.glob("w*_*.raw"):
        old.unlink()
    for old in OUT_DIR.glob("w*_*.wav"):
        old.unlink()

    samples, root = make_sine(N, ROOT_HZ)
    packed = struct.pack(
        f"<{len(samples)}h",
        *(int(round(clamp(x) * 32767.0)) for x in samples),
    )

    roots = [
        "# wave_id root_hz loop — identical 260 Hz sines on every id\n"
        "# Pitch = note_Hz / 260. C4 ≈ 1×, C5 = 2×. Same pitch on every\n"
        "# MIDI key means wave_id/root routing is broken.\n"
    ]

    print(f"Writing {NWAVES} × {N} samples @ {root:.6g} Hz → {OUT_DIR}")
    for wid in range(NWAVES):
        path = OUT_DIR / f"w{wid}_sine_c4.raw"
        path.write_bytes(packed)
        roots.append(f"{wid} {root:.6g} loop\n")

    (OUT_DIR / "roots.txt").write_text("".join(roots), encoding="utf-8")
    print(f"wrote {OUT_DIR / 'roots.txt'}")
    print("Done. GUI Load raw bank expects w0_*.raw … w255_*.raw")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
