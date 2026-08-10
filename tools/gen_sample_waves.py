#!/usr/bin/env python3
"""Generate 8 SAMPLE *synthetic* test instruments at 48 kHz.

Prefer real ~60 s evolving music for the ship bank (audibly non-repeating):
  python3 tools/build_sample48_music.py

Hardware one-note loops (tiled cells):
  python3 tools/build_sample48_real.py

This script remains for offline synth-only regeneration (no downloads).
"""

Writes per instrument:
  - wN_<name>_head.i32  — 256-sample int32 LE attack (card bank)
  - wN_<name>_body.i16  — int16 LE sustain / tail (host → UAC)
  - wN_<name>.wav       — short audition (head + body preview)
  - roots.txt           — `<id> <root_hz> loop|oneshot`

Loop bodies are constant-level, seamlessly loopable; card NoteEnv shapes amp.
Oneshot bodies bake a musical decay to silence and do not wrap on the host.

Native root ~C4: exact ET C4 does not close an integer number of periods in
the loop cell, so pitched content is 260 Hz and roots.txt reports 260.
"""

from __future__ import annotations

import math
import struct
import wave
from pathlib import Path

SR = 48000
ATTACK = 256
# Long body for host UAC soak tests (host RAM; card only holds the ring).
BODY_SECONDS = 60
BODY_LOOP = SR * BODY_SECONDS  # 2_880_000
# Seamless synth cell; tiled to BODY_LOOP. Period must close at ROOT_HZ.
BODY_CELL = 9600  # 200 ms
assert BODY_LOOP % BODY_CELL == 0
# Oneshot tails: long enough to hear decay; then silence (no wrap).
ONESHOT_SECONDS = 4.0
ONESHOT_LEN = int(SR * ONESHOT_SECONDS)
# ~C4. Must be a multiple of 5 Hz so BODY_CELL * f / SR is an integer.
ROOT_HZ = 260.0
OUT = Path(__file__).resolve().parents[1] / "cmi_control/waves/sample48"
# Audition WAV: head + this many body samples (not the full minute).
WAV_BODY_SAMPLES = SR * 2

TWO_PI = 2.0 * math.pi


def clamp(x: float, lo: float = -1.0, hi: float = 1.0) -> float:
    return lo if x < lo else hi if x > hi else x


def wrap01(x: float) -> float:
    return x - math.floor(x)


def poly_blep(t: float, dt: float) -> float:
    """Polynomial band-limited step residual (0..1 phase, dt = freq/SR)."""
    if t < dt:
        t = t / dt
        return t + t - t * t - 1.0
    if t > 1.0 - dt:
        t = (t - 1.0) / dt
        return t * t + t + t + 1.0
    return 0.0


def blep_saw(phase: float, dt: float) -> float:
    return (2.0 * phase - 1.0) - poly_blep(phase, dt)


def blep_pulse(phase: float, dt: float, width: float) -> float:
    y = 1.0 if phase < width else -1.0
    y += poly_blep(phase, dt)
    y -= poly_blep(wrap01(phase - width), dt)
    return y


def soft_sat(x: float, drive: float = 1.4) -> float:
    return math.tanh(x * drive)


def fade_in(i: int, n: int) -> float:
    if n <= 1:
        return 1.0
    x = i / (n - 1)
    return x * x * (3.0 - 2.0 * x)


def crossfade_loop(buf: list[float], n_xfade: int = 64) -> None:
    """Hide tiny endpoint discontinuity (detuned / noise loops)."""
    n = len(buf)
    if n_xfade <= 0 or n_xfade * 2 >= n:
        return
    for i in range(n_xfade):
        a = i / n_xfade
        a = a * a * (3.0 - 2.0 * a)
        head = buf[i]
        tail = buf[n - n_xfade + i]
        mix = head * a + tail * (1.0 - a)
        buf[i] = mix
        buf[n - n_xfade + i] = mix


def hash_noise(i: int, seed: float = 12.9898) -> float:
    x = math.sin(i * seed + 78.233) * 43758.5453
    return (x - math.floor(x)) * 2.0 - 1.0


# ---------------------------------------------------------------------------
# Loop instruments — constant level, seamless over BODY_CELL.
# ---------------------------------------------------------------------------


def voice_supersaw(i: int, freq: float = ROOT_HZ) -> float:
    """Seven detuned BLEP saws — classic analog stack."""
    dets = (-0.11, -0.07, -0.03, 0.0, 0.03, 0.07, 0.12)
    gains = (0.12, 0.14, 0.16, 0.22, 0.16, 0.14, 0.12)
    s = 0.0
    for d, g in zip(dets, gains):
        f = freq * (2.0 ** (d / 12.0))
        dt = f / SR
        ph = wrap01(i * dt)
        s += g * blep_saw(ph, dt)
    return soft_sat(s * 0.85, 1.15)


def voice_brass(i: int, freq: float = ROOT_HZ) -> float:
    """Bright odd-harmonic brass with slow lip vibrato (period = cell)."""
    ph = wrap01(i * freq / SR)
    dt = freq / SR
    saw = blep_saw(ph, dt)
    odd = 0.0
    for h, a in ((1, 1.0), (3, 0.42), (5, 0.20), (7, 0.10), (9, 0.05)):
        odd += a * math.sin(TWO_PI * h * ph)
    vib = 1.0 + 0.012 * math.sin(TWO_PI * i / BODY_CELL)
    s = 0.55 * saw + 0.45 * odd
    return soft_sat(s * vib * 0.72, 1.35)


def voice_pad(i: int, freq: float = ROOT_HZ) -> float:
    """Detuned sine + triangle choir pad with slow chorus."""
    s = 0.0
    for cents, g in ((-9.0, 0.26), (-3.0, 0.22), (0.0, 0.30), (3.0, 0.22), (9.0, 0.26)):
        f = freq * (2.0 ** (cents / 1200.0))
        ph = wrap01(i * f / SR)
        tri = 4.0 * abs(ph - 0.5) - 1.0
        s += g * (0.7 * math.sin(TWO_PI * ph) + 0.3 * tri)
    s += 0.16 * math.sin(TWO_PI * (freq * 0.5) * i / SR)
    ch = 1.0 + 0.05 * math.sin(TWO_PI * i / BODY_CELL)
    return soft_sat(s * ch * 0.7, 1.05)


def voice_organ(i: int, freq: float = ROOT_HZ) -> float:
    """Hammond-ish drawbars: 16' 8' 4' 2-2/3' 2' 1-3/5'."""
    feet = (
        (0.5, 0.7),
        (1.0, 0.9),
        (2.0, 0.75),
        (3.0, 0.45),
        (4.0, 0.35),
        (6.0, 0.2),
    )
    s = 0.0
    for mult, g in feet:
        f = freq * mult
        if f >= SR * 0.48:
            continue
        s += g * math.sin(TWO_PI * f * i / SR)
    leslie = 1.0 + 0.06 * math.sin(TWO_PI * i / (BODY_CELL / 2))
    return soft_sat(s * 0.45 * leslie, 1.1)


def voice_organ_attack(i: int, freq: float = ROOT_HZ) -> float:
    t = i / SR
    click = hash_noise(i, 45.12) * math.exp(-t * 220.0) * 0.35
    return clamp(voice_organ(i, freq) * fade_in(i, 48) + click)


# ---------------------------------------------------------------------------
# Oneshot instruments — evolve and die; host does not wrap.
# Absolute sample index from note start (head uses 0..ATTACK-1).
# ---------------------------------------------------------------------------


def voice_pluck_raw(i: int, freq: float = ROOT_HZ) -> float:
    """Karplus-ish partials with inharmonic stretch + decay."""
    t = i / SR
    s = 0.0
    for n, a in ((1, 0.55), (2, 0.22), (3, 0.14), (5, 0.08), (7, 0.045), (9, 0.025)):
        f = freq * n * (1.0 + 0.0012 * (n - 1))
        damp = math.exp(-t * (2.8 + 1.1 * n))
        s += a * damp * math.sin(TWO_PI * f * i / SR)
    return soft_sat(s * 0.85, 1.15)


def voice_pluck_attack(i: int, freq: float = ROOT_HZ) -> float:
    t = i / SR
    burst = hash_noise(i) * math.exp(-t * 200.0) * 0.6
    tone = voice_pluck_raw(i, freq) * (1.0 - math.exp(-t * 110.0))
    return clamp(burst + tone)


def voice_pluck_body(i: int, freq: float = ROOT_HZ) -> float:
    """Continue string after the head; absolute index = ATTACK + i."""
    return voice_pluck_raw(ATTACK + i, freq)


def voice_snare_raw(i: int) -> float:
    """Noise hit + short membrane ping — unpitched percussion."""
    t = i / SR
    n = hash_noise(i, 19.191)
    n2 = hash_noise(i // 2, 7.7)
    body = (0.65 * n + 0.35 * n2) * math.exp(-t * 9.5)
    # Mild resonant "snap"
    snap = math.sin(TWO_PI * 180.0 * t) * math.exp(-t * 55.0) * 0.45
    snap += math.sin(TWO_PI * 320.0 * t) * math.exp(-t * 70.0) * 0.25
    return soft_sat(body + snap, 1.6)


def voice_snare_attack(i: int) -> float:
    return clamp(voice_snare_raw(i))


def voice_snare_body(i: int) -> float:
    return voice_snare_raw(ATTACK + i)


def voice_sub808_raw(i: int, freq: float = ROOT_HZ) -> float:
    """808-style sine with pitch drop + click."""
    t = i / SR
    # f(t)=freq*(1+1.8*e^{-28t}) → phase = freq*(t + 1.8/28*(1-e^{-28t}))
    phase = freq * (t + (1.8 / 28.0) * (1.0 - math.exp(-28.0 * t)))
    body = math.sin(TWO_PI * phase)
    body = soft_sat(body * 1.05, 1.25)
    click = 0.0
    if i < 160:
        click = (1.0 - i / 160.0) * 0.5 * hash_noise(i, 0.31)
    env = math.exp(-t * 2.4)
    return clamp((0.9 * body + click) * env)


def voice_sub808_attack(i: int, freq: float = ROOT_HZ) -> float:
    return voice_sub808_raw(i, freq)


def voice_sub808_body(i: int, freq: float = ROOT_HZ) -> float:
    return voice_sub808_raw(ATTACK + i, freq)


def voice_epiano_raw(i: int, freq: float = ROOT_HZ) -> float:
    """2-op FM Rhodes / bell with decaying index."""
    t = i / SR
    ratio = 14.0
    mod_f = freq * ratio
    if mod_f > SR * 0.4:
        ratio = 7.0
        mod_f = freq * ratio
    idx = 2.4 * math.exp(-t * 5.5)
    mod_s = math.sin(TWO_PI * mod_f * t)
    car = math.sin(TWO_PI * freq * t + idx * mod_s)
    # Soft hammer
    click = 0.0
    if i < 100:
        click = (1.0 - i / 100.0) * 0.28 * math.sin(TWO_PI * 1800.0 * t)
    env = math.exp(-t * 2.8)
    return soft_sat((0.85 * car + click) * env, 1.15)


def voice_epiano_attack(i: int, freq: float = ROOT_HZ) -> float:
    return clamp(voice_epiano_raw(i, freq))


def voice_epiano_body(i: int, freq: float = ROOT_HZ) -> float:
    return voice_epiano_raw(ATTACK + i, freq)


# body_fn(i) for loops uses cell-local i; for oneshots uses body-local i.
# head_fn optional. mode is "loop" | "oneshot".
INSTRUMENTS = [
    (0, "supersaw", ROOT_HZ, "loop", lambda i: voice_supersaw(i, ROOT_HZ), None),
    (1, "brass", ROOT_HZ, "loop", lambda i: voice_brass(i, ROOT_HZ), None),
    (
        2,
        "pluck",
        ROOT_HZ,
        "oneshot",
        lambda i: voice_pluck_body(i, ROOT_HZ),
        lambda i: voice_pluck_attack(i, ROOT_HZ),
    ),
    (3, "pad", ROOT_HZ, "loop", lambda i: voice_pad(i, ROOT_HZ), None),
    (
        4,
        "snare",
        ROOT_HZ,
        "oneshot",
        voice_snare_body,
        voice_snare_attack,
    ),
    (
        5,
        "sub808",
        ROOT_HZ,
        "oneshot",
        lambda i: voice_sub808_body(i, ROOT_HZ),
        lambda i: voice_sub808_attack(i, ROOT_HZ),
    ),
    (
        6,
        "epiano",
        ROOT_HZ,
        "oneshot",
        lambda i: voice_epiano_body(i, ROOT_HZ),
        lambda i: voice_epiano_attack(i, ROOT_HZ),
    ),
    (
        7,
        "organ",
        ROOT_HZ,
        "loop",
        lambda i: voice_organ(i, ROOT_HZ),
        lambda i: voice_organ_attack(i, ROOT_HZ),
    ),
]


def make_head(body_fn, head_fn) -> list[float]:
    out: list[float] = []
    for i in range(ATTACK):
        if head_fn is not None:
            s = head_fn(i)
        else:
            s = body_fn(i) * fade_in(i, 72)
        out.append(clamp(s))
    # Blend into body[0] — host phase starts at 0 after the card attack head.
    body0 = body_fn(0)
    for i in range(32):
        idx = ATTACK - 32 + i
        a = i / 31.0
        out[idx] = clamp(out[idx] * (1.0 - a) + body0 * a)
    return out


def make_body_cell(body_fn) -> list[float]:
    out = [clamp(body_fn(i)) for i in range(BODY_CELL)]
    peak = max(abs(x) for x in out) or 1.0
    target = 10 ** (-1.0 / 20.0)
    g = target / peak
    out = [clamp(x * g) for x in out]
    crossfade_loop(out, 96)
    return out


def make_oneshot_body(body_fn) -> list[float]:
    out = [clamp(body_fn(i)) for i in range(ONESHOT_LEN)]
    peak = max(abs(x) for x in out) or 1.0
    target = 10 ** (-1.0 / 20.0)
    g = target / peak
    # Fade last 8 ms to exact zero so a stuck note stays silent after the hit.
    fade_n = int(0.008 * SR)
    for i, x in enumerate(out):
        y = x * g
        if i >= ONESHOT_LEN - fade_n:
            a = (ONESHOT_LEN - 1 - i) / max(fade_n - 1, 1)
            y *= a
        out[i] = clamp(y)
    return out


def write_i32(path: Path, samples: list[float]) -> None:
    with path.open("wb") as f:
        for x in samples:
            f.write(struct.pack("<i", int(clamp(x) * 2147483647.0)))


def write_i16_tiled(path: Path, cell: list[float], n_tiles: int) -> None:
    """Write int16 LE by repeating a seamless cell (avoids a giant float list)."""
    cell_bytes = b"".join(struct.pack("<h", int(clamp(x) * 32767.0)) for x in cell)
    with path.open("wb") as f:
        for _ in range(n_tiles):
            f.write(cell_bytes)


def write_i16(path: Path, samples: list[float]) -> None:
    with path.open("wb") as f:
        for x in samples:
            f.write(struct.pack("<h", int(clamp(x) * 32767.0)))


def write_wav(path: Path, samples: list[float]) -> None:
    with wave.open(str(path), "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(
            b"".join(struct.pack("<h", int(clamp(x) * 32767.0)) for x in samples)
        )


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for p in OUT.glob("w*_*.*"):
        p.unlink()

    n_tiles = BODY_LOOP // BODY_CELL
    body_mib = (BODY_LOOP * 2) / (1024 * 1024)
    print(
        f"loop body: {BODY_SECONDS}s = {BODY_LOOP} samples "
        f"({body_mib:.1f} MiB int16), cell={BODY_CELL} x {n_tiles}"
    )
    print(f"oneshot body: {ONESHOT_SECONDS:g}s = {ONESHOT_LEN} samples")

    roots_lines = [
        "# wave_id root_hz loop|oneshot — native pitch; oneshot bodies do not wrap\n"
    ]
    for idx, name, root_hz, mode, body_fn, head_fn in INSTRUMENTS:
        head = make_head(body_fn, head_fn)

        head_path = OUT / f"w{idx}_{name}_head.i32"
        body_path = OUT / f"w{idx}_{name}_body.i16"
        wav_path = OUT / f"w{idx}_{name}.wav"

        write_i32(head_path, head)

        if mode == "oneshot":
            body = make_oneshot_body(body_fn)
            write_i16(body_path, body)
            preview_n = min(WAV_BODY_SAMPLES, len(body))
            write_wav(wav_path, head + body[:preview_n])
            peak = max(abs(x) for x in body) if body else 0.0
            print(
                f"w{idx}_{name}: root={root_hz:g} Hz {mode} "
                f"head={ATTACK} body={len(body)} peak={peak:.3f}"
            )
        else:
            cell = make_body_cell(body_fn)
            write_i16_tiled(body_path, cell, n_tiles)
            preview: list[float] = []
            while len(preview) < WAV_BODY_SAMPLES:
                preview.extend(cell)
            write_wav(wav_path, head + preview[:WAV_BODY_SAMPLES])
            peak = max(abs(x) for x in cell)
            print(
                f"w{idx}_{name}: root={root_hz:g} Hz {mode} "
                f"head={ATTACK} body={BODY_LOOP} peak={peak:.3f}"
            )

        roots_lines.append(f"{idx} {root_hz:.6g} {mode}\n")

    (OUT / "roots.txt").write_text("".join(roots_lines), encoding="utf-8")
    print(f"wrote {OUT / 'roots.txt'}")


if __name__ == "__main__":
    main()
