#!/usr/bin/env python3
"""Measure c:vq\r round trips on Linux using only the Python standard library.

Stop MAS first: this script must be the only process using the RS485 adapter.
Defaults match mas/voicebd.h and its Linux devices.h: /dev/ttyUSB0, 3000000.
No note commands, resets, or USB BODY traffic are sent. A failed exchange stops
the run so a late response cannot be mistaken for the next request's response.
"""

import argparse
import csv
import fcntl
import math
import os
from pathlib import Path
import select
import statistics
import sys
import termios
import time
import tty

REQUEST = b"c:vq\r"
FRAME_SIZE = 61


def validate(frame):
    """Match voicebd.cpp's binary vq checks; the protocol has no CRC."""
    if len(frame) != FRAME_SIZE or frame[:4] != b"\xa5\x5aC\x0c" or frame[-1:] != b"\n":
        raise ValueError("invalid 61-byte vq frame header/terminator")
    capacity = int.from_bytes(frame[6:8], "little")
    if not 0 < capacity <= 8191:
        raise ValueError("invalid ring capacity")
    for i in range(8):
        at = 12 + 6 * i
        free = frame[at + 1] | ((frame[at + 2] & 31) << 8)
        refill = (frame[at + 2] >> 5) | (frame[at + 3] << 3) | ((frame[at + 4] & 1) << 11)
        active = (frame[4] | frame[5]) & (1 << i)
        if free > capacity or refill > 3840 or (active and frame[at] == 255):
            raise ValueError(f"invalid voice {i} record")
    return frame[8]


def exchange(fd, timeout):
    # Measure from immediately before host write to receipt of the full reply.
    start = time.perf_counter_ns()
    deadline = time.monotonic() + timeout
    sent = 0
    while sent < len(REQUEST):
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([], [fd], [], remaining)[1]:
            raise TimeoutError("request write timed out")
        try:
            sent += os.write(fd, REQUEST[sent:])
        except BlockingIOError:
            continue
    raw = bytearray()
    while len(raw) < FRAME_SIZE:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
            raise TimeoutError(f"received {len(raw)}/61 bytes; raw={raw.hex()}")
        try:
            chunk = os.read(fd, FRAME_SIZE - len(raw))
        except BlockingIOError:
            continue
        if not chunk:
            raise OSError("serial device disconnected")
        raw.extend(chunk)
    elapsed_ms = (time.perf_counter_ns() - start) / 1e6
    try:
        sequence = validate(raw)
    except ValueError as exc:
        raise ValueError(f"{exc}; raw={raw.hex()}") from exc
    return elapsed_ms, sequence, raw[4], raw[5]


def percentile(values, percent):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * percent / 100) - 1)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyUSB0", help="MAS Linux RS485 port (default: %(default)s)")
    parser.add_argument("--baud", type=int, default=3000000)
    parser.add_argument("--count", type=int, default=1000, help="measured exchanges (default: %(default)s)")
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--timeout-ms", type=float, default=100)
    parser.add_argument("--gap-ms", type=float, default=1, help="pause between exchanges, outside RTT")
    parser.add_argument("--csv", type=Path, help="save each measured RTT and status masks to a new CSV file")
    parser.add_argument("--latency-ms", type=int, choices=range(1, 256), metavar="1..255",
                        help="explicitly set the adapter's Linux latency_timer (may need permission); otherwise just report it")
    args = parser.parse_args()
    if (args.count <= 0 or args.warmup < 0 or not math.isfinite(args.timeout_ms)
            or args.timeout_ms <= 0 or not math.isfinite(args.gap_ms) or args.gap_ms < 0):
        parser.error("count/timeout must be positive; warmup/gap must be nonnegative")
    speed = getattr(termios, f"B{args.baud}", None)
    if speed is None:
        parser.error(f"this OS does not expose B{args.baud}; run on the Linux mainframe")

    samples = []
    fd = None
    saved = None
    csv_file = None
    failed = 0
    interrupted = False
    phase = "setup"
    try:
        if args.csv:
            csv_file = args.csv.open("x", newline="")
        writer = csv.writer(csv_file) if csv_file else None
        if writer:
            writer.writerow(("sample", "rtt_ms", "sequence", "active_mask", "pending_mask"))
        fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.ioctl(fd, termios.TIOCEXCL)
        saved = termios.tcgetattr(fd)
        tty.setraw(fd, termios.TCSANOW)
        settings = termios.tcgetattr(fd)
        settings[2] &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | getattr(termios, "CRTSCTS", 0))
        settings[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
        settings[4] = settings[5] = speed
        termios.tcsetattr(fd, termios.TCSANOW, settings)
        applied = termios.tcgetattr(fd)
        if applied[4] != speed or applied[5] != speed:
            raise OSError("serial driver did not accept the baud setting")
        timer = Path("/sys/bus/usb-serial/devices") / Path(args.port).resolve().name / "latency_timer"
        if args.latency_ms is not None:
            timer.write_text(f"{args.latency_ms}\n")
            if int(timer.read_text().strip()) != args.latency_ms:
                raise OSError("latency_timer setting did not read back correctly")
        latency = timer.read_text().strip() if timer.exists() else "not exposed"
        print(f"Port: {args.port}; baud: {args.baud}; format: 8N1; latency_timer: {latency}")
        if latency != "not exposed" and int(latency) != 1:
            print("Note: MAS requests a 1 ms latency_timer. Use --latency-ms 1 to match it (requires write permission).")
        print(f"Wire-time minimum for 5-byte request + 61-byte reply: {66 * 10 * 1000 / args.baud:.3f} ms")
        print("RTT includes host/USB scheduling, card processing, turnaround and full reply reception.")
        print("MAS must be stopped. This test sends only c:vq followed by a single CR.", flush=True)
        # Discard old input once, then require a quiet port before measuring.
        termios.tcflush(fd, termios.TCIFLUSH)
        if select.select([fd], [], [], 0.1)[0]:
            raise ValueError("unsolicited serial input; stop other users of this bus and retry")
        previous = None
        for i in range(args.warmup + args.count):
            phase = f"warmup {i + 1}" if i < args.warmup else f"sample {i - args.warmup + 1}"
            if select.select([fd], [], [], 0)[0]:
                raise ValueError("unexpected input between requests; possible competing reader/writer or extra reply")
            elapsed, sequence, active, pending = exchange(fd, args.timeout_ms / 1000)
            if previous is not None and sequence != (previous + 1) % 256:
                raise ValueError(f"vq sequence jumped {previous} -> {sequence}; possible competing client, stale reply or card reset")
            previous = sequence
            if i >= args.warmup:
                samples.append(elapsed)
                if writer:
                    writer.writerow((len(samples), f"{elapsed:.6f}", sequence, f"0x{active:02x}", f"0x{pending:02x}"))
                if len(samples) % 100 == 0:
                    print(f"{len(samples)}/{args.count}: last={elapsed:.3f} ms; max={max(samples):.3f} ms", flush=True)
            if args.gap_ms:
                time.sleep(args.gap_ms / 1000)
    except KeyboardInterrupt:
        interrupted = True
        print("\nInterrupted; reporting completed samples.")
    except (OSError, ValueError) as exc:
        failed = 1
        print(f"FAIL during {phase}: {exc}\nStopped to avoid timing a stale response as a new reply.", file=sys.stderr)
    finally:
        if fd is not None:
            try:
                if saved is not None:
                    termios.tcsetattr(fd, termios.TCSANOW, saved)
            finally:
                os.close(fd)
        if csv_file:
            csv_file.close()

    print(f"Valid measured replies: {len(samples)}/{args.count}; failures: {failed}")
    if samples:
        print(f"RTT ms: min={min(samples):.3f} median={statistics.median(samples):.3f} "
              f"mean={statistics.mean(samples):.3f} p95={percentile(samples, 95):.3f} "
              f"p99={percentile(samples, 99):.3f} max={max(samples):.3f}")
    if not failed and not interrupted:
        print("PASS: every requested exchange returned a valid, sequential vq frame. Hardware note-off handling is not tested.")
    return 130 if interrupted else failed


if __name__ == "__main__":
    sys.exit(main())
