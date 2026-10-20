#!/usr/bin/env python3
"""Exercise the shared simulator without a physical sound device."""
import os
from pathlib import Path
import subprocess
import select
import termios
import time
import tempfile

ROOT = Path(__file__).resolve().parents[3]
CHANNEL = ROOT / "channel"
TESTS = Path(__file__).resolve().parent
prefix = os.environ.get("PORTAUDIO_PREFIX") or next(
    (p for p in ("/opt/local", "/opt/homebrew", "/usr/local")
     if Path(p, "include/portaudio.h").exists()), None)
subprocess.run(["make", "-C", CHANNEL, "-j4"], check=True)
with tempfile.TemporaryDirectory(prefix="channel-simulator-tests-") as directory:
    work = Path(directory)
    compiler = work / "berry"
    subprocess.run(["make", "-C", ROOT / "berry_compiler", "-j4",
                    "BUILD_DIR=" + str(work / "compiler"), "OUTPUT=" + str(compiler)], check=True)
    program = work / "envelope.bec"
    subprocess.run([compiler, ROOT / "berry_compiler/examples/channel_envelope.be", "-o", program], check=True)
    objects = sorted(p for p in (CHANNEL / "bin").rglob("*.o")
                     if p.name != "main.cpp.o" and (CHANNEL / p.relative_to(CHANNEL / "bin").with_suffix("")).is_file())
    executable = work / "simulator_test"
    flags = ["-I" + str(CHANNEL), "-I" + str(CHANNEL / "berry_runtime")]
    if prefix:
        flags += ["-I" + str(Path(prefix) / "include")]
    subprocess.run([os.environ.get("HOST_CXX", "c++"), "-std=c++17", "-O2", "-g",
                    "-Wall", "-Wextra", "-Werror", "-pthread", *flags, TESTS / "simulator_test.cpp",
                    *objects, "-o", executable], check=True)
    subprocess.run([executable, program], check=True, timeout=30)

    process = subprocess.Popen([executable, "--main-loop"], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    clients = []
    paths = []
    try:
        for label in ("USB", "RS485"):
            line = process.stdout.readline().strip()
            assert line.startswith(label + ": "), line
            paths.append(line.split(": ", 1)[1])
            clients.append(os.open(paths[-1], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK))
        usb, serial = clients

        def receive(fd, count):
            output = bytearray()
            deadline = time.monotonic() + 3
            while len(output) < count:
                assert select.select([fd], [], [], max(0, deadline - time.monotonic()))[0], output
                output.extend(os.read(fd, 4096))
            return bytes(output)

        ready = bytearray()
        while not ready.endswith(b"\r\n"):
            ready.extend(receive(serial, 1))
        assert b"ready" in ready
        upload = bytes([2, 1, 7, 2, 0, 0, 0, 16, 32])
        for byte in upload:
            os.write(usb, bytes([byte]))
        assert receive(usb, 3) == bytes([7, 2, 0])
        os.write(serial, b"c:g 1 12\rc:g 1 13\r")
        assert receive(serial, 16) == b"[C] ok\r\n[C] ok\r\n"
        assert not select.select([usb, serial], [], [], 0.05)[0]
        print("PASS actual main loop: independent PTYs, complete ordered replies, no repeated input")

        # ---- reopen USB with the same flush used by the local voicebd client ----

        def reopen_usb(fd):
            os.close(fd)
            clients.remove(fd)
            fd = os.open(paths[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            clients.append(fd)
            termios.tcflush(fd, termios.TCIOFLUSH)
            os.write(fd, upload)
            reply = receive(fd, 3)
            assert reply == bytes([7, 2, 0]), reply
            return fd

        # ---- finish the current I/O pass before checking a connection boundary --

        def serial_barrier():
            os.write(serial, b"c:clear\r")
            expected = b"[C] ok:clear rt=1\r\n"
            assert receive(serial, len(expected)) == expected

        for _ in range(3):
            serial_barrier()
            usb = reopen_usb(usb)

        os.write(usb, bytes([1, 8, 0, 1, 0, 12]))
        assert not select.select([usb], [], [], 0.05)[0]
        usb = reopen_usb(usb)

        os.write(usb, bytes([2, 1]))
        serial_barrier()
        usb = reopen_usb(usb)

        partial_upload = bytes([2, 1, 7, 4, 0, 0, 0, 10, 20])
        os.write(usb, partial_upload)
        assert not select.select([usb], [], [], 0.05)[0]
        usb = reopen_usb(usb)

        os.write(usb, upload)
        serial_barrier()
        usb = reopen_usb(usb)
        assert not select.select([usb], [], [], 0.05)[0]

        os.write(serial, b"c:g 1 ")
        usb = reopen_usb(usb)
        os.write(serial, b"12\r")
        assert receive(serial, 8) == b"[C] ok\r\n"
        print("PASS USB reconnect: repeated opens, fault recovery, partial frames, interrupted uploads, stale replies, RS485 independence")
    finally:
        process.terminate()
        process.wait(timeout=5)
        for fd in clients:
            os.close(fd)
