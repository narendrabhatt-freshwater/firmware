#!/usr/bin/env python3
"""Build sample48 as long pure sine tones for polyphony listening tests.

Each slot is a different musical pitch (wide spacing) so simultaneous notes
are easy to tell apart. Bodies are seamless period-tiled 60 s loops at
constant level; card NoteEnv still shapes amp.

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

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "cmi_control/waves/sample48"

# Widely spaced pure tones — hold a chord and each pitch is obvious.
# (slot, name, hz) — name used in filenames; roots.txt gets exact hz.
BANK = [
    (0, "sine_a3", 220.000000),  # A3
    (1, "sine_c4", 261.625565),  # C4
    (2, "sine_e4", 329.627557),  # E4
    (3, "sine_g4", 391.995436),  # G4
    (4, "sine_c5", 523.251131),  # C5
    (5, "sine_e5", 659.255114),  # E5
    (6, "sine_g5", 783.990872),  # G5
    (7, "sine_c6", 1046.502261),  # C6
]


def clamp(x: float, lo: float = -1.0, hi: float = 1.0) -> float:
    return lo if x < lo else hi if x > hi else x


def period_samples(hz: float) -> int:
    """Nearest sample count for an integer number of cycles (seamless loop)."""
    # Prefer ~200 ms cell made of whole cycles.
    target = int(0.2 * SR)
    cycles = max(1, int(round(target * hz / SR)))
    # Exact period in samples may be fractional; use cycles * SR / hz rounded.
    n = int(round(cycles * SR / hz))
    # Ensure at least one full cycle.
    if n < 2:
        n = max(2, int(round(SR / hz)))
    return n


def make_sine_cell(hz: float) -> list[float]:
    n = period_samples(hz)
    # Recompute hz so exactly `cycles` periods fit in n samples.
    cycles = max(1, int(round(n * hz / SR)))
    # phase advance so sin(2π k cycles / n) closes.
    out = [
        TARGET_PEAK * math.sin(TWO_PI * cycles * i / n) for i in range(n)
    ]
    return out


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


def make_head(hz: float) -> list[float]:
    """Attack head: same sine with short fade-in."""
    out: list[float] = []
    for i in range(ATTACK):
        s = TARGET_PEAK * math.sin(TWO_PI * hz * i / SR)
        if i < 48:
            a = i / 47.0
            a = a * a * (3.0 - 2.0 * a)
            s *= a
        out.append(clamp(s))
    # Blend toward body phase at sample ATTACK (body starts at phase 0 of cell).
    # Body cell[0] is 0 for a sine that starts at 0 — fine.
    body0 = 0.0
    for i in range(32):
        idx = ATTACK - 32 + i
        a = i / 31.0
        out[idx] = clamp(out[idx] * (1.0 - a) + body0 * a)
    return out


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for p in OUT.glob("w*_*.*"):
        p.unlink()

    roots = [
        "# wave_id root_hz loop — pure sine polyphony test bank\n"
        "# Each slot is a different pitch; hold a chord to hear each note.\n"
    ]
    sources = [
        "sample48 pure-sine polyphony bank",
        "=================================",
        "",
        "Rebuild: python3 tools/build_sample48_sines.py",
        "Long seamless sine loops at widely spaced pitches so polyphony",
        "is easy to hear (no rich harmonics / no evolving beds).",
        "",
        "Slot  name      Hz",
        "----  --------  ----------",
    ]

    for slot, name, hz in BANK:
        cell = make_sine_cell(hz)
        head = make_head(hz)
        # Continuous sine from t=0 for head (fade only); body is seamless cell.
        # Re-make head from continuous phase matching cell start better:
        # body phase 0 = sin(0)=0. Head ends blended to 0.

        write_i32(OUT / f"w{slot}_{name}_head.i32", head)
        write_i16_tiled(OUT / f"w{slot}_{name}_body.i16", cell, BODY_LEN)

        preview: list[float] = []
        while len(preview) < WAV_PREVIEW:
            preview.extend(cell)
        write_wav(OUT / f"w{slot}_{name}.wav", head + preview[:WAV_PREVIEW])

        # Effective root = cycles * SR / len(cell) (exact loop pitch).
        cycles = max(1, int(round(len(cell) * hz / SR)))
        root = cycles * SR / len(cell)

        roots.append(f"{slot} {root:.6g} loop\n")
        sources.append(f"w{slot:<3} {name:<9} {root:.4f} Hz")
        print(
            f"w{slot}_{name}: {root:.4f} Hz cell={len(cell)} "
            f"body={BODY_LEN} ({BODY_SECONDS}s loop)"
        )

    (OUT / "roots.txt").write_text("".join(roots), encoding="utf-8")
    (OUT / "SOURCES.txt").write_text("\n".join(sources) + "\n", encoding="utf-8")
    print(f"wrote {OUT / 'roots.txt'}")


if __name__ == "__main__":
    main()
