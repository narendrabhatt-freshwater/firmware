#!/bin/sh
set -eu

exec python3 - "$@" <<'PY'
import argparse
import array
import fcntl
import os
import select
import statistics
import sys
import termios
import time
import tty

parser = argparse.ArgumentParser(
    prog="script.sh",
    description="Measure complete RS485 version-query round trips on macOS. "
                "CSV goes to stdout; summary goes to stderr. Stop voicebdtest/screen first.")
parser.add_argument("count", nargs="?", type=int, default=200)
parser.add_argument("--baud", type=int, default=921600,
                    help="must match the flashed firmware (default: 921600)")
parser.add_argument("--port", default="/dev/cu.usbserial-BG03CSYB")
args = parser.parse_args()
if sys.platform != "darwin":
    parser.error("this script configures the macOS serial adapter")
if args.count < 1 or args.baud < 1:
    parser.error("count and baud must be positive")

fd = -1
try:
    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd, termios.TCSANOW)
    mode = termios.tcgetattr(fd)
    mode[2] &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.CRTSCTS)
    mode[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
    mode[4] = termios.B9600
    mode[5] = termios.B9600
    mode[6][termios.VMIN] = 0
    mode[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, mode)
    IOSSIOSPEED = 0x80000000 | (array.array("L").itemsize << 16) | (ord("T") << 8) | 2
    fcntl.ioctl(fd, IOSSIOSPEED, array.array("L", [args.baud]))
    termios.tcflush(fd, termios.TCIFLUSH)

    print(f"{args.port}, {args.baud} baud, {args.count} queries", file=sys.stderr)
    print("query,round_trip_us")
    timings = []
    for i in range(args.count):
        start = time.perf_counter()
        deadline = start + 0.100
        if os.write(fd, b"v\r") != 2:
            raise RuntimeError("incomplete query write")
        reply = b""
        while len(reply) < 5:
            remaining = deadline - time.perf_counter()
            if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
                raise RuntimeError(f"query {i + 1}: incomplete reply after 100 ms: {reply!r}")
            chunk = os.read(fd, 5 - len(reply))
            if not chunk:
                raise RuntimeError("serial port closed during reply")
            reply += chunk
        elapsed = (time.perf_counter() - start) * 1_000_000
        if reply != b"0001\n":
            raise RuntimeError(f"query {i + 1}: unexpected reply {reply!r}")
        timings.append(elapsed)
        print(f"{i + 1},{elapsed:.3f}")
        if i + 1 < args.count:
            time.sleep((5000 + (i * 137) % 4000) / 1_000_000)

    ordered = sorted(timings)
    p95 = ordered[(95 * len(ordered) + 99) // 100 - 1]
    print(f"Valid replies: {len(timings)}/{args.count}", file=sys.stderr)
    print(f"Minimum: {min(timings) / 1000:.3f} ms", file=sys.stderr)
    print(f"Average: {statistics.mean(timings) / 1000:.3f} ms", file=sys.stderr)
    print(f"Median:  {statistics.median(timings) / 1000:.3f} ms", file=sys.stderr)
    print(f"95th percentile: {p95 / 1000:.3f} ms", file=sys.stderr)
    print(f"Maximum: {max(timings) / 1000:.3f} ms", file=sys.stderr)
    print(f"Above 1.5 ms: {sum(t > 1500 for t in timings)}", file=sys.stderr)
except (OSError, termios.error, RuntimeError) as error:
    sys.exit(str(error))
finally:
    if fd >= 0:
        os.close(fd)
PY
