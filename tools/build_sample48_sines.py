#!/usr/bin/env python3
"""Build sample48 as long pure C4 sines for polyphony listening tests.

All 8 slots share the SAME sine @ ~261.63 Hz with the SAME root. Heard pitch
comes only from host/card note Hz (rate = note_hz / root). A chord should be
obviously different notes of one clear sine — if every key still sounds like
the same pitch, polyphony routing is broken (not the sample bank).

Usage:
  python3 tools/build_sample48_sines.py
"""

from __future__ import annotations

import math
import struct
import wave
from pathlib import Path

SR = 48000
ATTACK = 256
BODY_SECONDS = 60
BODY_LEN = SR * BODY_SECONDS
WAV_PREVIEW = SR * 2
TARGET_PEAK = 10 ** (-1.0 / 20.0)
TWO_PI = 2.0 * math.pi
# Exact loop: 261.625565 does not close an integer period in a short cell;
# use 260 Hz (multiple of 5) so a 9600-sample cell is seamless, matching
# the historical sample48 root convention.
ROOT_HZ = 260.0

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "cmi_control/waves/sample48"

# Same sine / same root on every slot — polyphony = MIDI/note Hz only.
BANK = [(i, f"sine_c4", ROOT_HZ) for i in range(8)]


def clamp(x: float, lo: float = -1.0, hi: float = 1.0) -> float:
    return lo if x < lo else hi if x > hi else x


def make_head(hz: float) -> list[float]:
    """First ATTACK samples of the same sine the body continues."""
    out: list[float] = []
    for i in range(ATTACK):
        s = TARGET_PEAK * math.sin(TWO_PI * hz * i / SR)
        if i < 48:
            a = i / 47.0
            a = a * a * (3.0 - 2.0 * a)
            s *= a
        out.append(clamp(s))
    return out


def make_sine_cell(hz: float, phase0: int = 0) -> list[float]:
    # 200 ms @ 260 Hz = exactly 52 cycles in 9600 samples.
    n = 9600
    cycles = int(round(n * hz / SR))
    return [
        TARGET_PEAK * math.sin(TWO_PI * cycles * (i + phase0) / n)
        for i in range(n)
    ]


def write_i32(path: Path, samples: list[float]) -> None:
    with path.open("wb") as f:
        for v in samples:
            f.write(struct.pack("<i", int(clamp(v) * 2147483647.0)))


def write_i16_tiled(path: Path, cell: list[float], total: int) -> None:
    cell_b = b"".join(struct.pack("<h", int(clamp(v) * 32767.0)) for v in cell)
    n_full = total // len(cell)
    rem = total % len(cell)
    with path.open("wb") as f:
        for _ in range(n_full):
            f.write(cell_b)
        if rem:
            f.write(cell_b[: rem * 2])


def write_wav(path: Path, samples: list[float]) -> None:
    with wave.open(str(path), "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(
            b"".join(struct.pack("<h", int(clamp(v) * 32767.0)) for v in samples)
        )


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for p in OUT.glob("w*_*.*"):
        p.unlink()

    cell = make_sine_cell(ROOT_HZ, ATTACK)
    head = make_head(ROOT_HZ)
    cycles = int(round(len(cell) * ROOT_HZ / SR))
    root = cycles * SR / len(cell)

    roots = [
        "# wave_id root_hz loop — identical C4 sines; pitch = note_hz/root\n"
        "# If a MIDI chord still sounds like one pitch, polyphony is broken.\n"
    ]
    sources = [
        "sample48 identical-sine polyphony bank",
        "======================================",
        "",
        "Rebuild: python3 tools/build_sample48_sines.py",
        f"All slots: pure sine @ {root:.4f} Hz, root={root:.4f}.",
        "Host/card pitch with rate = note_hz / root. A C-E-G chord must",
        "sound like three clear pitches; same pitch on every key = bug.",
        "Head is sine[0:256]; body continues from sample 256 (one timeline).",
        "",
        "Slot  name     Hz",
        "----  -------- ----------",
    ]

    for slot, name, _hz in BANK:
        fname = f"w{slot}_{name}"
        write_i32(OUT / f"{fname}_head.i32", head)
        write_i16_tiled(OUT / f"{fname}_body.i16", cell, BODY_LEN)
        preview: list[float] = []
        while len(preview) < WAV_PREVIEW:
            preview.extend(cell)
        write_wav(OUT / f"{fname}.wav", head + preview[:WAV_PREVIEW])
        roots.append(f"{slot} {root:.6g} loop\n")
        sources.append(f"w{slot:<3} {name:<8} {root:.4f} Hz")
        print(f"{fname}: root={root:.4f} Hz cell={len(cell)} body={BODY_LEN}")

    (OUT / "roots.txt").write_text("".join(roots), encoding="utf-8")
    (OUT / "SOURCES.txt").write_text("\n".join(sources) + "\n", encoding="utf-8")
    print(f"wrote {OUT / 'roots.txt'}")


if __name__ == "__main__":
    main()
