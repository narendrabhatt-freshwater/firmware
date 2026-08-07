#!/usr/bin/env python3
"""Generate 8 × 32 KiB int16 LE mono synth one-shots for Channel Card wave banks.

Design notes
------------
- Length: 16384 samples (32768 bytes) — full AXI slot.
- Fundamental period is 128 samples so MIDI rate = Hz×128 yields pitch ≈ Hz
  (filter track uses rate/128).
- Nominal playback for audition without pitch shift: wN 48000 (optional).
- One-shot: exponential / ADSR-style envelopes fade to silence by the end.
"""

from __future__ import annotations

import math
import struct
import wave
from pathlib import Path

N = 16384
PERIOD = 128  # samples per fundamental cycle
F0 = 1.0 / PERIOD  # cycles per sample
OUT_DIR = Path(__file__).resolve().parents[1] / "apps" / "control_gui" / "waves"


def clamp(x: float, lo: float = -1.0, hi: float = 1.0) -> float:
    return lo if x < lo else hi if x > hi else x


def env_adsr(
    i: int,
    n: int,
    *,
    a: float = 0.002,
    d: float = 0.08,
    s: float = 0.55,
    r: float = 0.35,
) -> float:
    """Simple ADSR over [0,1) of the buffer. Times are fractions of length."""
    t = i / n
    if t < a:
        return t / a if a > 0 else 1.0
    if t < a + d:
        u = (t - a) / d
        return 1.0 + (s - 1.0) * u
    if t < 1.0 - r:
        return s
    u = (t - (1.0 - r)) / r if r > 0 else 1.0
    return s * (1.0 - u)


def env_exp(i: int, n: int, *, tau: float = 0.22, attack: float = 0.004) -> float:
    t = i / n
    if t < attack:
        return t / attack
    return math.exp(-(t - attack) / tau)


def softsat(x: float, drive: float = 1.4) -> float:
    return math.tanh(x * drive) / math.tanh(drive)


def poly_blep(t: float, dt: float) -> float:
    """t in [0,1), dt = increment per sample — naive polyBLEP for saw/square."""
    if t < dt:
        x = t / dt
        return x + x - x * x - 1.0
    if t > 1.0 - dt:
        x = (t - 1.0) / dt
        return x * x + x + x + 1.0
    return 0.0


def bl_saw(phase: float, dt: float) -> float:
    t = phase - math.floor(phase)
    return (2.0 * t - 1.0) - poly_blep(t, dt)


def bl_square(phase: float, dt: float, duty: float = 0.5) -> float:
    t = phase - math.floor(phase)
    y = 1.0 if t < duty else -1.0
    y += poly_blep(t, dt)
    t2 = t - duty
    if t2 < 0.0:
        t2 += 1.0
    y -= poly_blep(t2, dt)
    return y


def write_raw(path: Path, samples: list[float]) -> None:
    assert len(samples) == N
    peak = max(abs(x) for x in samples) or 1.0
    scale = 0.92 * 32767.0 / peak
    data = bytearray()
    for x in samples:
        v = int(round(clamp(x) * scale))
        data += struct.pack("<h", v)
    path.write_bytes(data)
    # Optional WAV sibling for audition in a DAW (not uploaded to the card).
    wav_path = path.with_suffix(".wav")
    with wave.open(str(wav_path), "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(48000)
        w.writeframes(bytes(data))
    print(f"  {path.name:28s}  {len(data)} B  peak={peak:.3f}")


def gen_w0_supersaw() -> list[float]:
    """Detuned saw stack + mild LP sweep — classic analog lead attack."""
    dets = [-0.012, -0.005, 0.0, 0.006, 0.014]
    out: list[float] = []
    phases = [0.1 * k for k in range(len(dets))]
    for i in range(N):
        t = i / N
        # Closing filter feel: high harmonics early, darker later.
        bright = 0.55 + 0.45 * math.exp(-t * 4.5)
        s = 0.0
        for k, d in enumerate(dets):
            dt = F0 * (1.0 + d)
            phases[k] += dt
            s += bl_saw(phases[k], dt)
        s = softsat(s * 0.28 * bright, 1.6)
        s *= env_adsr(i, N, a=0.003, d=0.12, s=0.45, r=0.40)
        out.append(s)
    return out


def gen_w1_epiano() -> list[float]:
    """2-op FM electric-piano / bell pluck."""
    out: list[float] = []
    for i in range(N):
        t = i / N
        # Carrier : modulator ≈ 1 : 14 (Rhodes-ish), index decays.
        idx = 3.2 * math.exp(-t * 6.5)
        mod = math.sin(2.0 * math.pi * F0 * 14.0 * i)
        car = math.sin(2.0 * math.pi * F0 * i + idx * mod)
        # Soft hammer click
        click = 0.0
        if i < 80:
            click = (1.0 - i / 80.0) * 0.35 * math.sin(2.0 * math.pi * 0.18 * i)
        s = softsat(0.85 * car + click, 1.2)
        s *= env_exp(i, N, tau=0.28, attack=0.0015)
        out.append(s)
    return out


def gen_w2_pwm_square() -> list[float]:
    """Pulse with slow PWM — thick 70s square lead."""
    out: list[float] = []
    phase = 0.0
    for i in range(N):
        t = i / N
        duty = 0.5 + 0.28 * math.sin(2.0 * math.pi * 2.5 * t)
        dt = F0
        phase += dt
        s = bl_square(phase, dt, duty=duty)
        # Add sub octave
        s = 0.72 * s + 0.28 * math.sin(2.0 * math.pi * (F0 * 0.5) * i)
        s = softsat(s * 0.7, 1.5)
        s *= env_adsr(i, N, a=0.004, d=0.10, s=0.5, r=0.38)
        out.append(s)
    return out


def gen_w3_noise_snare() -> list[float]:
    """Filtered noise body + short tonal ping — snare / clap energy."""
    out: list[float] = []
    # xorshift-ish LCG
    state = 0xA5A5F00D

    def rnd() -> float:
        nonlocal state
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        return (state / 0xFFFFFFFF) * 2.0 - 1.0

    lp = 0.0
    hp = 0.0
    for i in range(N):
        t = i / N
        n = rnd()
        # Soft LPF that opens then closes
        cut = 0.08 + 0.55 * math.exp(-t * 8.0)
        lp += cut * (n - lp)
        # Mild HPF for body
        hp = 0.97 * (hp + lp - (out[-1] if out else 0.0))
        body = hp
        # Tonal ping (membranes)
        ping = math.sin(2.0 * math.pi * F0 * 2.0 * i) * math.exp(-t * 28.0)
        ping += 0.5 * math.sin(2.0 * math.pi * F0 * 3.2 * i) * math.exp(-t * 35.0)
        s = 0.75 * body + 0.55 * ping
        s *= env_exp(i, N, tau=0.12, attack=0.0008)
        out.append(softsat(s, 1.8))
    return out


def gen_w4_brass() -> list[float]:
    """Brassy saw + odd harmonics with lip-buzz attack."""
    out: list[float] = []
    phase = 0.0
    for i in range(N):
        t = i / N
        dt = F0
        phase += dt
        saw = bl_saw(phase, dt)
        # Odd partials (brass-ish)
        odd = 0.0
        for h, amp in ((1, 1.0), (3, 0.45), (5, 0.22), (7, 0.12), (9, 0.06)):
            odd += amp * math.sin(2.0 * math.pi * F0 * h * i)
        buzz = 0.15 * math.sin(2.0 * math.pi * F0 * 0.5 * i) * math.exp(-t * 3.0)
        bright = 0.4 + 0.6 * (1.0 - math.exp(-t * 12.0))  # swell in
        s = softsat((0.55 * saw + 0.45 * odd) * bright + buzz, 1.7)
        s *= env_adsr(i, N, a=0.06, d=0.15, s=0.6, r=0.35)
        out.append(s)
    return out


def gen_w5_sub808() -> list[float]:
    """808-style sine pitch drop + click."""
    out: list[float] = []
    phase = 0.0
    for i in range(N):
        t = i / N
        # Instant pitch envelope: start ~2× fund, settle to fund
        pitch = 1.0 + 1.2 * math.exp(-t * 22.0)
        phase += F0 * pitch
        body = math.sin(2.0 * math.pi * phase)
        # Soft saturation for "punch"
        body = softsat(body * 1.1, 1.3)
        click = 0.0
        if i < 120:
            click = (1.0 - i / 120.0) * 0.55 * rnd_click(i)
        s = 0.9 * body + click
        s *= env_exp(i, N, tau=0.42, attack=0.001)
        out.append(s)
    return out


def rnd_click(i: int) -> float:
    # Deterministic high-freq click
    return math.sin(2.0 * math.pi * (0.31 + 0.02 * (i % 7)) * i)


def gen_w6_karplus() -> list[float]:
    """Karplus–Strong style plucked string (period = 128)."""
    buf = [0.0] * PERIOD
    state = 0xC0FFEE

    def rnd() -> float:
        nonlocal state
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        return (state / 0x7FFFFFFF) * 2.0 - 1.0

    for i in range(PERIOD):
        buf[i] = rnd()
    out: list[float] = []
    idx = 0
    prev = 0.0
    for i in range(N):
        y = buf[idx]
        # Light damping + one-pole average (string loss)
        avg = 0.5 * (y + prev)
        prev = y
        damp = 0.996 - 0.004 * (i / N)  # more loss over time
        buf[idx] = avg * damp
        idx = (idx + 1) % PERIOD
        # Soft body tone under the noise burst
        tone = 0.25 * math.sin(2.0 * math.pi * F0 * i) * env_exp(i, N, tau=0.35)
        s = softsat(0.85 * y + tone, 1.25)
        s *= env_adsr(i, N, a=0.001, d=0.05, s=0.55, r=0.45)
        out.append(s)
    return out


def gen_w7_organ() -> list[float]:
    """Hammond-ish drawbar additive (16'+8'+4'+2⅔'+2') with key click."""
    # Drawbar relative amps (rough 888000000 feel, truncated)
    partials = [
        (0.5, 0.9),   # 16'
        (1.0, 1.0),   # 8'
        (2.0, 0.85),  # 4'
        (3.0, 0.55),  # 2⅔'
        (4.0, 0.4),   # 2'
        (6.0, 0.22),  # 1⅓'
        (8.0, 0.15),  # 1'
    ]
    out: list[float] = []
    for i in range(N):
        t = i / N
        s = 0.0
        for h, a in partials:
            # Slight chorus on upper ranks
            det = 1.0 + (0.0015 * math.sin(2.0 * math.pi * 0.7 * t) if h >= 2 else 0.0)
            s += a * math.sin(2.0 * math.pi * F0 * h * det * i)
        # Key click
        if i < 90:
            s += (1.0 - i / 90.0) * 0.4 * math.sin(2.0 * math.pi * 0.22 * i)
        # Leslie-ish AM
        s *= 0.92 + 0.08 * math.sin(2.0 * math.pi * 5.5 * t)
        s = softsat(s * 0.22, 1.15)
        s *= env_adsr(i, N, a=0.01, d=0.08, s=0.7, r=0.30)
        out.append(s)
    return out


BANK = [
    ("w0_supersaw.raw", gen_w0_supersaw),
    ("w1_epiano.raw", gen_w1_epiano),
    ("w2_pwm_square.raw", gen_w2_pwm_square),
    ("w3_noise_snare.raw", gen_w3_noise_snare),
    ("w4_brass.raw", gen_w4_brass),
    ("w5_sub808.raw", gen_w5_sub808),
    ("w6_pluck_ks.raw", gen_w6_karplus),
    ("w7_organ.raw", gen_w7_organ),
]


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    # Remove older demo names so Load folder doesn't pick stale shorts.
    for old in OUT_DIR.glob("w*_*.raw"):
        old.unlink()
    for old in OUT_DIR.glob("w*_*.wav"):
        old.unlink()

    print(f"Writing {len(BANK)} × {N} samples ({N * 2} bytes) → {OUT_DIR}")
    for name, fn in BANK:
        write_raw(OUT_DIR / name, fn())
    print("Done. GUI Load folder expects w0_*.raw … w7_*.raw")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
