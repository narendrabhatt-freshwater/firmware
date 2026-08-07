#!/usr/bin/env python3
"""Load a raw int16 LE mono file into a Channel Card wave slot over USB CDC.

Usage:
  wave_load.py --port /dev/cu.usbmodemXXXX <slot> <file.raw>

Protocol (see docs/protocol.md):
  send:  wl <slot> <nbytes>\\r
  expect: ok:ready
  send:  raw bytes
  expect: ok:wave <slot> <nsamp>
"""

from __future__ import annotations

import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("pip install pyserial", file=sys.stderr)
    sys.exit(1)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True, help="CDC ACM device path")
    ap.add_argument("--baud", type=int, default=115200, help="ignored by ACM")
    ap.add_argument("slot", type=int, choices=range(0, 8))
    ap.add_argument("file", type=argparse.FileType("rb"))
    args = ap.parse_args()

    data = args.file.read()
    args.file.close()
    n = len(data)
    if n < 2 or (n & 1) or n > 32768:
        print(f"err: file size {n} (need even 2..32768)", file=sys.stderr)
        return 1

    ser = serial.Serial(args.port, args.baud, timeout=2.0)
    time.sleep(0.2)
    ser.reset_input_buffer()

    cmd = f"c:wl {args.slot} {n}\r".encode("ascii")
    ser.write(cmd)
    ser.flush()

    deadline = time.time() + 5.0
    ready = False
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            if b"ok:ready" in buf:
                ready = True
                break
            if b"err:" in buf:
                print(buf.decode("ascii", errors="replace"), file=sys.stderr)
                return 1
    if not ready:
        print("err: timeout waiting for ok:ready", file=sys.stderr)
        print(buf.decode("ascii", errors="replace"), file=sys.stderr)
        return 1

    # Stream payload in 512-byte chunks.
    off = 0
    while off < n:
        piece = data[off : off + 512]
        ser.write(piece)
        ser.flush()
        off += len(piece)

    deadline = time.time() + 10.0
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            if b"ok:wave" in buf:
                print(buf.decode("ascii", errors="replace").strip())
                return 0
            if b"err:" in buf:
                print(buf.decode("ascii", errors="replace"), file=sys.stderr)
                return 1

    print("err: timeout waiting for ok:wave", file=sys.stderr)
    print(buf.decode("ascii", errors="replace"), file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
