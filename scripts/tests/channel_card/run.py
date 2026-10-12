#!/usr/bin/env python3
#                                    __
#                                ___  \  \
#                           ___  \  \  \  \    _______
#                      ___  \  \  \  \  \__\__/_____  \
#                      \  \  \  \  \__\_______      \__\____
#                       \  \  \  \     ___ \  \________  \__/
#                        \  \  \__\___/_  \ \___/   \  \___
#                         \  \    ____  \  \________ \ ___/
#                          \__\__/ \  \  \____/  \  \
#                                   \  \     ___  \  \___
#                                    \__\___/ \  \ \____/
#                                            \  \
#                                               \  \___
#                  __                 _          \ ___/  _
#                 / _|_ __  ___  ___ | |____      ____ _| |_  ___  _ __
#                | |_| '_ |/ _ \/ __|| '_ \ \ /\ / / _` | __|/ _ \| '__|
#                |  _| |  |  __/\__ \| | | \ V  V / (_| | |_|  __/| |
#                |_| |_|   \___||___/|_| |_|\_/\_/ \__,_|\__|\___||_|
#
#             (C) 2 0 2 6   F r e s h w a t e r   I n s t r u m e n t s

# ---- sources and settings ---------------------------------------------------

"""Build isolated host tests; optional comparison with a pre-refactor snapshot."""
import argparse
import hashlib
import pathlib
import re
import os
import select
import signal
import time
import tty
import subprocess
import tempfile

TESTS = pathlib.Path(__file__).resolve().parent
REPO = TESTS.parents[2]
ROOT = REPO / "channel_card"


def run(args, **kwargs):
    result = subprocess.run([str(a) for a in args], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, **kwargs)
    if result.returncode:
        raise RuntimeError(result.stdout.decode(errors="replace") +
                           result.stderr.decode(errors="replace"))
    return result.stdout


def build(root, work, main, legacy=False):
    sources = ["app/" + name + ".cpp" for name in
               ("audio", "channel", "filter", "oscillator", "voice", "samples", "stream", "usb")]
    if not legacy:
        sources = re.findall(r"app/\w+\.cpp", (root / "app/sources.mk").read_text())
        sources += ["app/audio_dac.cpp"]
    sources += ["berry_runtime/vm.c", "berry_runtime/vm_source.c",
                "berry_runtime/berry_backend.c", "berry_runtime/berry_runtime_modtab.c"]
    sources += [str(p.relative_to(root)) for p in sorted(
        (root / "berry_runtime/third_party/berry/src").glob("*.c"))]
    sources += ["tests/" + main + ".cpp", "tests/backend_model.cpp"]
    executable = work / main
    args = ["make", "-f", TESTS / "Makefile", "ROOT=" + str(root), "-j4", "OBJDIR=" + str(work / "obj"),
            "OUTPUT=" + str(executable), "SOURCES=" + " ".join(sources)]
    if legacy:
        args += ["COMMON_FLAGS=-DLEGACY_API -O2 -g -Wall -MMD -MP"]
    run(args)
    return executable


def check_boundaries():
    sources = re.findall(r"app/\w+\.cpp", (ROOT / "app/sources.mk").read_text())
    forbidden = re.compile(r"\b(?:HAL_\w+|DWT|CoreDebug|__disable_irq|__get_PRIMASK|"
                           r"__set_PRIMASK|__DMB|__arm__|__thumb__)\b")
    for source in sources:
        text = (ROOT / source).read_text()
        text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
        assert not forbidden.search(text), f"Hardware leaked into {source}"
        assert not re.search(r'#include\s+["<](?:main|board|cs4304|board_stm32|usb_device)\.h', text)
    for source in ("board_stm32.cpp", "usb_device.cpp"):
        assert "process_usb_messages(" not in (ROOT / "app" / source).read_text()
    print("PASS application/platform dependency boundaries")


def pty_check(work):
    sources = re.findall(r"app/\w+\.cpp", (ROOT / "app/sources.mk").read_text())
    sources += ["berry_runtime/vm.c", "berry_runtime/vm_source.c", "berry_runtime/berry_backend.c",
                "berry_runtime/berry_runtime_modtab.c"]
    sources += [str(p.relative_to(ROOT)) for p in sorted(
        (ROOT / "berry_runtime/third_party/berry/src").glob("*.c"))]
    sources += ["sim/serialdevices.cpp", "sim/clock.cpp", "tests/pty_main.cpp"]
    exe = work / "pty_sim"
    run(["make", "-f", TESTS / "Makefile", "ROOT=" + str(ROOT), "-j4", "OBJDIR=" + str(work / "obj"),
         "OUTPUT=" + str(exe), "SOURCES=" + " ".join(sources)])
    env = dict(os.environ, CHANNEL_TEST_DIRECTORY=str(work))
    child = subprocess.Popen([exe], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    opened = []
    try:
        deadline = time.monotonic() + 5
        while not (work / "rs485").exists():
            assert child.poll() is None, child.stderr.read().decode()
            assert time.monotonic() < deadline, "PTY startup timeout"
            time.sleep(0.01)
        def connect(name):
            fd = os.open(work / name, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            tty.setraw(fd)
            opened.append(fd)
            return fd
        def receive(fd, count):
            result = b""
            deadline = time.monotonic() + 3
            while len(result) < count:
                remaining = deadline - time.monotonic()
                assert remaining > 0 and select.select([fd], [], [], remaining)[0], "PTY reply timeout"
                result += os.read(fd, count - len(result))
            return result
        usb, rs = connect("usb"), connect("rs485")
        os.write(usb, bytes([1, 0, 0, 1, 0, 2]))
        assert receive(usb, 19)[:6] == bytes([6, 0, 1, 14, 0, 0])
        # Serial command service runs alongside an ordered USB diagnostic stream.
        os.write(usb, bytes([7, 0, 0, 3, 0, 10, 11, 12, 7, 0, 0, 0, 0]))
        os.write(rs, b"c:vm\r")
        assert receive(rs, 4) == b"[C] "
        probe = receive(usb, 18)
        assert probe[:6] == bytes([6, 0, 7, 13, 0, 0])
        assert int.from_bytes(probe[6:10], "little") == 3
        os.close(usb); opened.remove(usb)
        # Allow the production loop to observe close before reopening the PTY.
        time.sleep(0.05)
        usb = connect("usb")
        os.write(usb, bytes([1, 0, 0, 1, 0, 2]))
        assert receive(usb, 19)[:6] == bytes([6, 0, 1, 14, 0, 0])
    finally:
        for fd in opened:
            os.close(fd)
        child.send_signal(signal.SIGTERM)
        child.communicate(timeout=5)
        assert child.returncode == 0
    print("PASS isolated PTY simulator: USB/RS485 traffic and reconnect")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=pathlib.Path,
                        help="Captured pre-refactor channel_card source directory")
    args = parser.parse_args()
    check_boundaries()
    with tempfile.TemporaryDirectory(prefix="channel-tests-") as directory:
        work = pathlib.Path(directory)
        compiler = work / "berry"
        run(["make", "-C", REPO / "berry_compiler", "-j4",
             "BUILD_DIR=" + str(work / "compiler"), "OUTPUT=" + str(compiler)])
        program = work / "envelope.bec"
        run([compiler, REPO / "berry_compiler/examples/channel_envelope.be", "-o", program])
        trace = build(ROOT, work / "current", "application_trace")
        baseline = (build(args.baseline.resolve(), work / "baseline", "application_trace", True)
                    if args.baseline else None)
        for scenario in ("envelope", "oscillator", "fault"):
            fixture = work / (scenario + ".bec")
            source = (REPO / "berry_compiler/examples/channel_envelope.be" if scenario == "envelope"
                      else TESTS / (scenario + ".be"))
            run([compiler, source, "-o", fixture])
            expected = run([trace, fixture, 480])
            for size in (1, 7, 47, 48, 49, 96, 256, 511):
                actual = run([trace, fixture, size])
                assert actual == expected, f"{scenario} mismatch at partition {size}"
            print("PASS audio/state partitions:", scenario, hashlib.sha256(expected).hexdigest())
            if baseline:
                assert run([baseline, fixture, 48]) == expected, "Pre-refactor audio/state mismatch"
                print("PASS pre-refactor baseline:", scenario)
        serial = work / "serial_backend_test"
        run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "app"), "-I" + str(ROOT / "sim"),
             TESTS / "serial_backend_test.cpp", "-o", serial])
        run([serial])
        print("PASS real serial backend: partial writes, errors, queues, flush, disconnect")
        pty_check(work)
        for name in ("protocol_test", "boundary_test", "dac_trace"):
            exe = build(ROOT, work / "current", name)
            output = run([exe, program])
            if name in ("dac_trace", "protocol_test") and args.baseline:
                old = build(args.baseline.resolve(), work / "baseline", name, True)
                assert output == run([old, program]), "Pre-refactor " + name + " mismatch"
            print("PASS", name)


if __name__ == "__main__":
    main()
