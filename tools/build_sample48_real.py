#!/usr/bin/env python3
"""Build sample48 bank from real synthesizer WAV recordings.

Default source: Kenneth Reitz / Infinite State open sample pack
(https://github.com/kennethreitz/infinite-state-sample-pack) — hardware
synth C-note takes (Moog, JP-08, Monark, MicroBrute, TR-8, …) @ 48 kHz.

Writes cmi_control/waves/sample48/:
  wN_<name>_head.i32 / _body.i16 / .wav + roots.txt + SOURCES.txt

Usage:
  python3 tools/build_sample48_real.py              # download + build
  python3 tools/build_sample48_real.py --src DIR    # use existing pack root
"""

from __future__ import annotations

import argparse
import math
import struct
import subprocess
import sys
import wave
import zipfile
from pathlib import Path
from urllib.request import urlretrieve

SR = 48000
ATTACK = 256
BODY_LOOP_SECONDS = 60
BODY_LOOP = SR * BODY_LOOP_SECONDS
WAV_PREVIEW_BODY = SR * 2
TARGET_PEAK = 10 ** (-1.0 / 20.0)  # -1 dBFS

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "cmi_control/waves/sample48"
CACHE = REPO / ".cache" / "infinite-state-sample-pack"
PACK_ZIP_URL = (
    "https://github.com/kennethreitz/infinite-state-sample-pack/"
    "archive/refs/heads/master.zip"
)

# All eight slots are seamless sustain loops from held C-note takes.
# (slot, name, relative wav under Samples/, mode, hint_hz)
BANK = [
    (0, "moog_lead", "Moog Sub37/sub37-5-lead.wav", "loop", 261.63),
    (1, "jp08_lead", "Roland JP-08/jp08-2-lead.wav", "loop", 130.81),
    (2, "brute_bass", "Arturia MicroBrute/microbrute-3-bass.wav", "loop", 130.81),
    (3, "jp08_fifth", "Roland JP-08/jp08-6-fifth.wav", "loop", 65.41),
    (4, "mono_lead", "Korg Monolouge/monolouge-3-lead.wav", "loop", 130.81),
    (5, "mono_bass", "Korg Monolouge/monolouge-5-pure-bass.wav", "loop", 65.41),
    (6, "volca_lead", "Korg VolcaKeys/volcakeys-9-lead.wav", "loop", 261.63),
    (7, "monark_lead", "NI Monark/monark-2-lead.wav", "loop", 261.63),
]

FIXED_ROOT_SLOTS: set[int] = set()



def clamp(x: float, lo: float = -1.0, hi: float = 1.0) -> float:
    return lo if x < lo else hi if x > hi else x


def ensure_pack(src: Path | None) -> Path:
    if src is not None:
        samples = src / "Samples"
        if not samples.is_dir():
            raise SystemExit(f"no Samples/ under {src}")
        return src

    marker = CACHE / "Samples"
    if marker.is_dir():
        return CACHE

    CACHE.mkdir(parents=True, exist_ok=True)
    zip_path = CACHE.parent / "infinite-state-sample-pack.zip"
    print(f"downloading {PACK_ZIP_URL}")
    urlretrieve(PACK_ZIP_URL, zip_path)
    extract = CACHE.parent / "infinite-state-extract"
    if extract.exists():
        import shutil

        shutil.rmtree(extract)
    extract.mkdir(parents=True)
    with zipfile.ZipFile(zip_path) as zf:
        zf.extractall(extract)
    # archive-master/Samples → CACHE
    roots = list(extract.glob("*/Samples"))
    if not roots:
        raise SystemExit("zip missing Samples/")
    import shutil

    if CACHE.exists():
        shutil.rmtree(CACHE)
    shutil.move(str(roots[0].parent), str(CACHE))
    print(f"cached pack at {CACHE}")
    return CACHE


def load_wav_mono_f32(path: Path) -> tuple[int, list[float]]:
    """Load WAV as mono float32 @ native rate (ffmpeg if needed)."""
    # Prefer ffmpeg → exact 48k mono s16 for consistency.
    tmp = path.with_suffix(".mono48.wav")
    cmd = [
        "ffmpeg",
        "-y",
        "-v",
        "error",
        "-i",
        str(path),
        "-ac",
        "1",
        "-ar",
        str(SR),
        "-sample_fmt",
        "s16",
        str(tmp),
    ]
    subprocess.run(cmd, check=True)
    with wave.open(str(tmp), "rb") as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2
        assert w.getframerate() == SR
        n = w.getnframes()
        raw = w.readframes(n)
    tmp.unlink(missing_ok=True)
    samples = struct.unpack("<" + "h" * n, raw)
    return SR, [s / 32768.0 for s in samples]


def estimate_f0(x: list[float], hint_hz: float | None) -> float | None:
    """Autocorr f0 on a mid window. Returns None if unpitched / weak."""
    if len(x) < SR // 4:
        # short oneshot — use hint or skip
        return hint_hz
    a = int(0.25 * SR)
    b = min(len(x), a + int(0.6 * SR))
    seg = x[a:b]
    mean = sum(seg) / len(seg)
    seg = [v - mean for v in seg]
    n = len(seg)

    if hint_hz and hint_hz > 0:
        fmin = hint_hz / 1.6
        fmax = hint_hz * 1.6
    else:
        fmin, fmax = 55.0, 600.0
    minlag = max(2, int(SR / fmax))
    maxlag = min(n - 2, int(SR / fmin))
    best_r = -1.0
    best_lag = 0
    for lag in range(minlag, maxlag + 1):
        num = 0.0
        d1 = 0.0
        d2 = 0.0
        m = n - lag
        for i in range(m):
            a_ = seg[i]
            b_ = seg[i + lag]
            num += a_ * b_
            d1 += a_ * a_
            d2 += b_ * b_
        if d1 <= 1e-12 or d2 <= 1e-12:
            continue
        r = num / math.sqrt(d1 * d2)
        if r > best_r:
            best_r = r
            best_lag = lag
    if best_lag == 0 or best_r < 0.85:
        return hint_hz
    return SR / best_lag


def normalize(x: list[float]) -> list[float]:
    peak = max(abs(v) for v in x) or 1.0
    g = TARGET_PEAK / peak
    return [clamp(v * g) for v in x]


def crossfade_loop(buf: list[float], n_xfade: int) -> None:
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


def make_loop_cell(x: list[float], period: int) -> list[float]:
    """Take ~200 ms of sustain, snapped to `period`, crossfade ends."""
    # Prefer post-attack sustain.
    start = min(len(x) // 5, max(ATTACK, int(0.15 * SR)))
    target_len = max(period * 4, int(0.2 * SR))
    # Snap length to integer periods.
    n_per = max(1, int(round(target_len / period)))
    length = n_per * period
    if start + length > len(x):
        start = max(0, len(x) - length)
    if start + length > len(x):
        # Fall back: use whatever remains, pad by looping period.
        chunk = x[start:]
        while len(chunk) < length:
            chunk = chunk + chunk
        cell = chunk[:length]
    else:
        cell = list(x[start : start + length])
    cell = normalize(cell)
    crossfade_loop(cell, min(96, period // 2))
    return cell


def write_i32(path: Path, samples: list[float]) -> None:
    with path.open("wb") as f:
        for v in samples:
            f.write(struct.pack("<i", int(clamp(v) * 2147483647.0)))


def write_i16(path: Path, samples: list[float]) -> None:
    with path.open("wb") as f:
        for v in samples:
            f.write(struct.pack("<h", int(clamp(v) * 32767.0)))


def write_i16_tiled(path: Path, cell: list[float], n_tiles: int) -> None:
    cell_b = b"".join(struct.pack("<h", int(clamp(v) * 32767.0)) for v in cell)
    with path.open("wb") as f:
        for _ in range(n_tiles):
            f.write(cell_b)


def write_wav(path: Path, samples: list[float]) -> None:
    with wave.open(str(path), "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(
            b"".join(struct.pack("<h", int(clamp(v) * 32767.0)) for v in samples)
        )


def make_head(x: list[float]) -> list[float]:
    if len(x) < ATTACK:
        x = x + [0.0] * (ATTACK - len(x))
    head = normalize(x[:ATTACK])
    # Soft onset if the take starts hot (avoid click into card attack).
    for i in range(min(48, ATTACK)):
        a = i / 47.0
        a = a * a * (3.0 - 2.0 * a)
        head[i] *= a
    return head


def build_one(
    slot: int,
    name: str,
    wav: Path,
    mode: str,
    hint_hz: float | None,
) -> tuple[float, str]:
    _, x = load_wav_mono_f32(wav)
    x = normalize(x)
    if slot in FIXED_ROOT_SLOTS:
        root = float(hint_hz or 261.625565)
    else:
        f0 = estimate_f0(x, hint_hz)
        if mode == "oneshot" and f0 is None:
            root = float(hint_hz or 261.625565)
        else:
            root = float(f0 if f0 is not None else hint_hz or 261.625565)

    head = make_head(x)
    body_src = x[ATTACK:] if len(x) > ATTACK else [0.0]
    if not body_src:
        body_src = [0.0]

    head_path = OUT / f"w{slot}_{name}_head.i32"
    body_path = OUT / f"w{slot}_{name}_body.i16"
    wav_path = OUT / f"w{slot}_{name}.wav"
    write_i32(head_path, head)

    if mode == "oneshot":
        body = normalize(body_src)
        fade_n = min(len(body), int(0.01 * SR))
        for i in range(fade_n):
            a = (fade_n - 1 - i) / max(fade_n - 1, 1)
            body[len(body) - fade_n + i] *= a
        write_i16(body_path, body)
        preview = body[: min(len(body), WAV_PREVIEW_BODY)]
        write_wav(wav_path, head + preview)
        print(
            f"w{slot}_{name}: {mode} root={root:.2f} Hz "
            f"src={wav.name} body={len(body)} ({len(body)/SR:.2f}s)"
        )
    else:
        period = max(2, int(round(SR / root)))
        # Refine period so SR/period ≈ root
        period = max(2, int(round(SR / root)))
        cell = make_loop_cell(x, period)
        # Ensure cell length divides BODY_LOOP for clean tile count.
        # Trim to largest multiple of period that fits, then tile.
        n_tiles = max(1, BODY_LOOP // len(cell))
        # Rewrite body length exactly n_tiles * len(cell)
        write_i16_tiled(body_path, cell, n_tiles)
        preview: list[float] = []
        while len(preview) < WAV_PREVIEW_BODY:
            preview.extend(cell)
        write_wav(wav_path, head + preview[:WAV_PREVIEW_BODY])
        print(
            f"w{slot}_{name}: {mode} root={root:.2f} Hz "
            f"src={wav.name} cell={len(cell)} (period≈{period}) "
            f"body={n_tiles * len(cell)}"
        )

    return root, mode


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--src",
        type=Path,
        default=None,
        help="Path to infinite-state-sample-pack root (contains Samples/)",
    )
    args = ap.parse_args()

    if not shutil_which("ffmpeg"):
        print("ffmpeg required", file=sys.stderr)
        return 1

    pack = ensure_pack(args.src)
    samples_dir = pack / "Samples"
    OUT.mkdir(parents=True, exist_ok=True)
    for p in OUT.glob("w*_*.*"):
        p.unlink()

    roots_lines = [
        "# wave_id root_hz loop|oneshot — from real synth WAV (see SOURCES.txt)\n"
    ]
    source_lines = [
        "sample48 real-synth bank",
        "========================",
        "",
        "Source pack: Infinite State open sample pack",
        "  https://github.com/kennethreitz/infinite-state-sample-pack",
        "  Hardware synth C-note recordings (48 kHz). Redistribute per that",
        "  project's terms; rebuild with: python3 tools/build_sample48_real.py",
        "",
        "Slot  file                         mode     source WAV",
        "----  ---------------------------- -------- --------------------------------",
    ]

    for slot, name, rel, mode, hint in BANK:
        wav = samples_dir / rel
        if not wav.is_file():
            raise SystemExit(f"missing {wav}")
        root, mode_out = build_one(slot, name, wav, mode, hint)
        roots_lines.append(f"{slot} {root:.6g} {mode_out}\n")
        source_lines.append(
            f"w{slot:<3} {name:<28} {mode_out:<8} {rel}"
        )

    (OUT / "roots.txt").write_text("".join(roots_lines), encoding="utf-8")
    (OUT / "SOURCES.txt").write_text("\n".join(source_lines) + "\n", encoding="utf-8")
    print(f"wrote {OUT / 'roots.txt'}")
    print(f"wrote {OUT / 'SOURCES.txt'}")
    return 0


def shutil_which(cmd: str) -> bool:
    from shutil import which

    return which(cmd) is not None


if __name__ == "__main__":
    raise SystemExit(main())
