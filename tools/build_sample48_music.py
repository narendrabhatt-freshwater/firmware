#!/usr/bin/env python3
"""Build sample48 from ~1-minute continuously evolving musical downloads.

Each body is a unique ~60 s mono 48 kHz take (no short-cell tiling), so a
held note audibly changes over time. Mode is oneshot — after the minute the
host holds silence (proves the stream is not a tiny loop).

Sources are CC0 / CC-BY game-music tracks from OpenGameArt (see SOURCES.txt).

Usage:
  python3 tools/build_sample48_music.py
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import wave
from pathlib import Path
from urllib.request import urlretrieve

SR = 48000
ATTACK = 256
BODY_SECONDS = 60
BODY_LEN = SR * BODY_SECONDS
WAV_PREVIEW = SR * 2  # audition: head + 2 s
TARGET_PEAK = 10 ** (-1.0 / 20.0)
# Musical beds: MIDI C4 plays at recorded speed.
ROOT_HZ = 261.625565

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "cmi_control/waves/sample48"
CACHE = REPO / ".cache" / "sample48_music"

# (slot, name, local_cache_name, download_url, page_url, license)
BANK = [
    (
        0,
        "first_light",
        "first_light.wav",
        "https://opengameart.org/sites/default/files/first_light_particles_0.wav",
        "https://opengameart.org/content/first-light-particles-%E2%80%93-cc0-atmospheric-pianoambient-track",
        "CC0",
    ),
    (
        1,
        "budding",
        "budding.wav",
        "https://opengameart.org/sites/default/files/the_budding_of_consciousness.wav",
        "https://opengameart.org/content/the-budding-of-consciousness-%E2%80%93-cc0-ambient-minimalist-theme-yoiyami-blue-series-%E2%80%93-no4",
        "CC0",
    ),
    (
        2,
        "observing",
        "observing.ogg",
        "https://opengameart.org/sites/default/files/ObservingTheStar.ogg",
        "https://opengameart.org/content/another-space-background-track",
        "CC0",
    ),
    (
        3,
        "tragic",
        "tragic.ogg",
        "https://opengameart.org/sites/default/files/ambientmain_0.ogg",
        "https://opengameart.org/content/tragic-ambient-main-menu",
        "CC0",
    ),
    (
        4,
        "forest",
        "forest.ogg",
        "https://opengameart.org/sites/default/files/forest.ogg",
        "https://opengameart.org/content/creepy-forest-f",
        "CC0",
    ),
    (
        5,
        "cyberpunk",
        "cyberpunk.mp3",
        "https://opengameart.org/sites/default/files/Cyberpunk%20Moonlight%20Sonata%20v2.mp3",
        "https://opengameart.org/content/cyberpunk-moonlight-sonata",
        "CC0",
    ),
    (
        6,
        "snowfall",
        "snowfall.ogg",
        "https://opengameart.org/sites/default/files/Snowfall_0.ogg",
        "https://opengameart.org/content/snowfall",
        "CC0",
    ),
    (
        7,
        "bluespace",
        "bluespace.wav",
        "https://opengameart.org/sites/default/files/Blue%20Space%20v0_96.wav",
        "https://opengameart.org/content/blue-space",
        "CC-BY 3.0",
    ),
]


def clamp(x: float, lo: float = -1.0, hi: float = 1.0) -> float:
    return lo if x < lo else hi if x > hi else x


def which(cmd: str) -> bool:
    from shutil import which as w

    return w(cmd) is not None


def ensure_download(cache_name: str, url: str) -> Path:
    CACHE.mkdir(parents=True, exist_ok=True)
    dest = CACHE / cache_name
    if dest.is_file() and dest.stat().st_size > 1000:
        return dest
    print(f"  downloading {cache_name} ...")
    urlretrieve(url, dest)
    return dest


def ffmpeg_mono60(src: Path, dst: Path) -> None:
    """Decode → mono 48 kHz s16, first BODY_SECONDS, peak-normalize ~-1 dBFS."""
    cmd = [
        "ffmpeg",
        "-y",
        "-v",
        "error",
        "-i",
        str(src),
        "-t",
        str(BODY_SECONDS),
        "-ac",
        "1",
        "-ar",
        str(SR),
        "-sample_fmt",
        "s16",
        "-af",
        f"loudnorm=I=-16:TP=-1.5:LRA=11,atrim=0:{BODY_SECONDS},asetpts=PTS-STARTPTS",
        str(dst),
    ]
    # loudnorm needs two passes for best results; simpler peak norm is enough:
    cmd = [
        "ffmpeg",
        "-y",
        "-v",
        "error",
        "-i",
        str(src),
        "-t",
        str(BODY_SECONDS),
        "-ac",
        "1",
        "-ar",
        str(SR),
        "-af",
        "pan=mono|c0=0.5*c0+0.5*c1,dynaudnorm=f=150:g=15",
        "-sample_fmt",
        "s16",
        str(dst),
    ]
    # Some files are already mono — pan with 2ch fails. Use aformat instead.
    cmd = [
        "ffmpeg",
        "-y",
        "-v",
        "error",
        "-i",
        str(src),
        "-t",
        str(BODY_SECONDS),
        "-ac",
        "1",
        "-ar",
        str(SR),
        "-af",
        "dynaudnorm=f=150:g=15",
        "-c:a",
        "pcm_s16le",
        str(dst),
    ]
    subprocess.run(cmd, check=True)


def load_s16_mono(path: Path) -> list[float]:
    with wave.open(str(path), "rb") as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2
        assert w.getframerate() == SR
        n = w.getnframes()
        raw = w.readframes(n)
    samples = struct.unpack("<" + "h" * n, raw)
    return [s / 32768.0 for s in samples]


def normalize(x: list[float]) -> list[float]:
    peak = max(abs(v) for v in x) or 1.0
    g = TARGET_PEAK / peak
    return [clamp(v * g) for v in x]


def write_i32(path: Path, samples: list[float]) -> None:
    with path.open("wb") as f:
        for v in samples:
            f.write(struct.pack("<i", int(clamp(v) * 2147483647.0)))


def write_i16(path: Path, samples: list[float]) -> None:
    with path.open("wb") as f:
        for v in samples:
            f.write(struct.pack("<h", int(clamp(v) * 32767.0)))


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
    head = list(x[:ATTACK])
    for i in range(min(48, ATTACK)):
        a = i / 47.0
        a = a * a * (3.0 - 2.0 * a)
        head[i] *= a
    return head


def build_one(slot: int, name: str, src: Path) -> None:
    mono = CACHE / f"{name}_mono60.wav"
    ffmpeg_mono60(src, mono)
    x = normalize(load_s16_mono(mono))
    # Pad or trim to exactly BODY_LEN after the head split target.
    if len(x) < ATTACK + 1:
        raise SystemExit(f"{name}: decoded too short ({len(x)} samples)")
    if len(x) < BODY_LEN:
        x = x + [0.0] * (BODY_LEN - len(x))
    else:
        x = x[:BODY_LEN]

    head = make_head(x)
    body = x[ATTACK:]
    # Fade last 20 ms so oneshot end is clean.
    fade_n = int(0.02 * SR)
    for i in range(fade_n):
        a = (fade_n - 1 - i) / max(fade_n - 1, 1)
        body[len(body) - fade_n + i] *= a

    write_i32(OUT / f"w{slot}_{name}_head.i32", head)
    write_i16(OUT / f"w{slot}_{name}_body.i16", body)
    write_wav(OUT / f"w{slot}_{name}.wav", head + body[:WAV_PREVIEW])
    print(
        f"w{slot}_{name}: oneshot root={ROOT_HZ:.2f} Hz "
        f"body={len(body)} ({len(body)/SR:.1f}s) — unique, not tiled"
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--cache-only",
        action="store_true",
        help="Do not download; require files already in .cache/sample48_music",
    )
    args = ap.parse_args()

    if not which("ffmpeg"):
        print("ffmpeg required", file=sys.stderr)
        return 1

    OUT.mkdir(parents=True, exist_ok=True)
    for p in OUT.glob("w*_*.*"):
        p.unlink()

    roots = [
        "# wave_id root_hz oneshot — ~60s evolving music (not a tiled loop cell)\n"
    ]
    sources = [
        "sample48 evolving-music bank (~60 s each, continuously changing)",
        "================================================================",
        "",
        "Rebuild: python3 tools/build_sample48_music.py",
        "Bodies are unique full-length takes (no short seamless cell tiling).",
        "Host mode: oneshot — silence after ~60 s (proves stream is not looping).",
        "Root Hz fixed at C4 so MIDI C4 ≈ recorded pitch.",
        "",
        "Slot  name         license   page",
        "----  -----------  --------  --------------------------------",
    ]

    for slot, name, cache_name, url, page, lic in BANK:
        if args.cache_only:
            src = CACHE / cache_name
            if not src.is_file():
                raise SystemExit(f"missing cache file {src}")
        else:
            # Prefer already-downloaded /tmp copies if present (dev speed).
            tmp = Path("/tmp/fw_music") / cache_name
            if tmp.is_file() and tmp.stat().st_size > 1000:
                CACHE.mkdir(parents=True, exist_ok=True)
                dest = CACHE / cache_name
                if not dest.is_file():
                    dest.write_bytes(tmp.read_bytes())
                src = dest
            else:
                src = ensure_download(cache_name, url)
        build_one(slot, name, src)
        roots.append(f"{slot} {ROOT_HZ:.6g} oneshot\n")
        sources.append(f"w{slot:<3} {name:<12} {lic:<8}  {page}")

    (OUT / "roots.txt").write_text("".join(roots), encoding="utf-8")
    (OUT / "SOURCES.txt").write_text("\n".join(sources) + "\n", encoding="utf-8")
    print(f"wrote {OUT / 'roots.txt'}")
    print(f"wrote {OUT / 'SOURCES.txt'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
