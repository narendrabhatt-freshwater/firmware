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
import array
import math
import struct
import hashlib
import pathlib
import re
import os
import select
import signal
import time
import tty
import subprocess
import sys
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


def application_sources(root, build="sim"):
    makefile = (root / build / "Makefile").read_text().replace("\\\n", " ")
    match = re.search(r"^APP_SOURCES\s*:=\s*(.*)$", makefile, re.M)
    assert match, f"Missing application source list in {build}/Makefile"
    return match[1].split()


def build(root, work, main, legacy=False):
    sources = ["app/" + name + ".cpp" for name in
               ("audio", "channel", "filter", "oscillator", "voice", "samples", "stream", "usb")]
    if not legacy:
        sources = application_sources(root)
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
    sources = application_sources(ROOT)
    assert set(sources) <= set(application_sources(ROOT, "app"))
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


def portaudio_flags():
    prefix = os.environ.get("PORTAUDIO_PREFIX")
    if not prefix:
        prefix = next((p for p in ("/opt/local", "/opt/homebrew", "/usr/local")
                       if pathlib.Path(p, "include/portaudio.h").exists()), "")
    includes = ["-I" + prefix + "/include"] if prefix else []
    libraries = (["-L" + prefix + "/lib"] if prefix else []) + ["-lportaudio"]
    return includes, libraries


def pty_check(work):
    sources = application_sources(ROOT)
    sources += ["berry_runtime/vm.c", "berry_runtime/vm_source.c", "berry_runtime/berry_backend.c",
                "berry_runtime/berry_runtime_modtab.c"]
    sources += [str(p.relative_to(ROOT)) for p in sorted(
        (ROOT / "berry_runtime/third_party/berry/src").glob("*.c"))]
    sources += ["sim/serialdevices.cpp", "sim/clock.cpp", "sim/audio_device.cpp", "tests/fake_portaudio.cpp", "tests/pty_main.cpp"]
    exe = work / "pty_sim"
    run(["make", "-f", TESTS / "Makefile", "ROOT=" + str(ROOT), "-j4", "OBJDIR=" + str(work / "obj"),
         "OUTPUT=" + str(exe), "SOURCES=" + " ".join(sources),
         "EXTRA_INCLUDES=" + " ".join(portaudio_flags()[0]), "LDLIBS=-lm -pthread"])
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


def audio_check(work, compiler, envelope=False):
    """Play a quiet tone through the real device; observe its submitted buffers."""
    sources = application_sources(ROOT)
    sources += ["berry_runtime/vm.c", "berry_runtime/vm_source.c", "berry_runtime/berry_backend.c",
                "berry_runtime/berry_runtime_modtab.c"]
    sources += [str(p.relative_to(ROOT)) for p in sorted(
        (ROOT / "berry_runtime/third_party/berry/src").glob("*.c"))]
    sources += ["sim/serialdevices.cpp", "sim/clock.cpp", "tests/audio_pty_main.cpp"]
    exe = work / "audio_sim"
    run(["make", "-f", TESTS / "Makefile", "ROOT=" + str(ROOT), "-j4",
         "OBJDIR=" + str(work / "audio-obj"), "OUTPUT=" + str(exe),
         "SOURCES=" + " ".join(sources),
         "EXTRA_INCLUDES=" + " ".join(portaudio_flags()[0]),
         "LDLIBS=-lm -pthread " + " ".join(portaudio_flags()[1])])
    program = work / "audio.bec"
    fixture = (REPO / "berry_compiler/examples/channel_envelope.be" if envelope
               else TESTS / "oscillator.be")
    run([compiler, fixture, "-o", program])
    capture = work / "audio.capture"
    env = dict(os.environ, CHANNEL_TEST_DIRECTORY=str(work), CHANNEL_AUDIO_CAPTURE=str(capture))
    opened = []
    command_times = []
    with (work / "audio.log").open("wb") as log:
        child = subprocess.Popen([exe], env=env, stdout=subprocess.DEVNULL, stderr=log)
        try:
            deadline = time.monotonic() + 5
            while not (work / "rs485").exists():
                assert child.poll() is None, (work / "audio.log").read_text()
                assert time.monotonic() < deadline, "audio simulator startup timeout"
                time.sleep(0.01)
            def connect(name):
                fd = os.open(work / name, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                tty.setraw(fd)
                opened.append(fd)
                return fd
            usb, rs = connect("usb"), connect("rs485")
            def receive(fd, count):
                result = b""
                deadline = time.monotonic() + 3
                while len(result) < count:
                    remaining = deadline - time.monotonic()
                    assert remaining > 0 and select.select([fd], [], [], remaining)[0], "audio test reply timeout"
                    result += os.read(fd, count - len(result))
                return result
            def frame(kind, target, payload, session=0):
                packet = struct.pack("<BBBH", kind, target, session, len(payload)) + payload
                deadline = time.monotonic() + 3
                while packet:
                    remaining = deadline - time.monotonic()
                    assert remaining > 0 and select.select([], [usb], [], remaining)[1]
                    try:
                        packet = packet[os.write(usb, packet):]
                    except BlockingIOError:
                        pass
            def request(kind, target, payload):
                frame(kind, target, payload)
                header = receive(usb, 5)
                typ, tgt, req, size = struct.unpack("<BBBH", header)
                assert (typ, tgt, req) == (6, target, kind), header
                reply = receive(usb, size)
                assert reply[0] == 0, reply
                return reply
            def upload(kind, target, data):
                request(3, target, struct.pack("<BI", kind, len(data)))
                for offset in range(0, len(data), 1020):
                    assert request(4, target, struct.pack("<I", offset) + data[offset:offset + 1020]) == b"\0"
            def command(text, expect_reply=True):
                start = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
                os.write(rs, text.encode() + b"\r")
                # The current parser treats note on/off as one-way events.
                # Confirm their effects using vq and rendered PCM.
                if not expect_reply:
                    return start
                reply = b""
                while not reply.endswith(b"\r\n"):
                    reply += receive(rs, 1)
                command_times.append((time.clock_gettime_ns(time.CLOCK_MONOTONIC) - start) / 1e6)
                assert reply.startswith(b"[C] ok"), reply
                return start
            def status():
                start = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
                os.write(rs, b"c:vq\r")
                reply = receive(rs, 61)
                command_times.append((time.clock_gettime_ns(time.CLOCK_MONOTONIC) - start) / 1e6)
                assert reply[:4] == b"\xa5\x5aC\x0e" and reply[-1:] == b"\n", reply
                return reply
            assert request(1, 0, b"\x02")[1:4] == bytes([2, 8, 1])
            # A 1 kHz sample has exactly 48 samples per cycle. Matching attack
            # and BODY phases make one-cycle RMS expose the envelope shape.
            def sample_data(start, count):
                return bytes(round(24 * math.sin(2 * math.pi * (i % 48) / 48)) & 255
                             for i in range(start, start + count))
            upload(1, 0, sample_data(0, 512) if envelope else bytes(512))
            wave = bytes(round(24 * math.sin(2 * math.pi * i / 256)) & 255 for i in range(256))
            upload(2, 0, wave)
            upload(3, 0, program.read_bytes())
            command("c:ar 0 440")
            time.sleep(0.15)
            note_on = command("c:n0 on 0 69 127 @1", expect_reply=False)
            # Only one BODY may be unacknowledged. A fresh vq after its ack
            # supplies authoritative per-voice credit before the next write.
            acknowledged = int.from_bytes(status()[10:12], "little")
            expected_ack = acknowledged
            body_position = 480  # 512-sample attack minus its 32-sample overlap.
            saw_active = False
            note_off = None
            end = time.monotonic() + 1.6
            while time.monotonic() < end:
                state = status()
                active = bool(state[4] & 1)
                pending = bool(state[5] & 1)
                saw_active |= active
                acknowledged = int.from_bytes(state[10:12], "little")
                free = int.from_bytes(state[14:18], "little") & 0xfff
                if (active or pending) and acknowledged == expected_ack and free >= 1024:
                    frame(2, 0, sample_data(body_position, 1024) if envelope else bytes(1024), 1)
                    body_position += 1024
                    expected_ack = (expected_ack + 1) & 0xffff
                if note_off is None and time.clock_gettime_ns(time.CLOCK_MONOTONIC) - note_on > 800_000_000:
                    note_off = command("c:n0 off", expect_reply=False)
                time.sleep(0.001)
            assert saw_active and not (status()[4] & 1), "note did not activate/release"
            clear = command("c:clear")
            time.sleep(0.15)
        finally:
            for fd in opened:
                os.close(fd)
            if child.poll() is None:
                child.send_signal(signal.SIGTERM)
            child.wait(timeout=5)
            assert child.returncode == 0, (work / "audio.log").read_text()
    assert not (work / "usb").exists() and not (work / "rs485").exists()
    data = capture.read_bytes()
    blocks = []
    offset = 0
    while offset < len(data):
        timestamp, size = struct.unpack_from("<QI", data, offset)
        offset += 12
        pcm = array.array("i", data[offset:offset + size])
        offset += size
        assert len(pcm) * 4 == size and len(pcm) % 2 == 0
        assert pcm[::2] == pcm[1::2], "left/right mismatch"
        blocks.append((timestamp, pcm[::2]))
    assert len(blocks) > 20 and offset == len(data)
    assert all(not any(pcm) for stamp, pcm in blocks if stamp < note_on)
    steady = [sample for stamp, pcm in blocks
              if note_on + 200_000_000 < stamp < note_off for sample in pcm]
    assert len(steady) > 4800 and max(steady) > 0 and min(steady) < 0, "no rendered tone"
    crossings = sum(a <= 0 < b for a, b in zip(steady, steady[1:]))
    frequency = crossings * 48000 / len(steady)
    assert abs(frequency - (1000 if envelope else 440)) < 5, frequency
    release_limit = 350_000_000 if envelope else 150_000_000
    after_release = [sample for stamp, pcm in blocks if stamp > note_off + release_limit for sample in pcm]
    assert after_release and not any(after_release), "note-off did not produce silence"
    assert any(stamp > clear for stamp, _ in blocks), "device stopped requesting samples"
    first_sound = next(stamp for stamp, pcm in blocks if any(pcm))
    rms = math.sqrt(sum((v / 2147483648) ** 2 for v in steady) / len(steady))
    if envelope:
        check_live_envelope(blocks, rms)
    print(f"PASS native audio: USB uploads/BODY, RS485 note-on/off, {frequency:.1f} Hz, "
          f"stereo match, RMS {rms:.4f}, release to silence, clean shutdown")
    print(f"Audio timing: command-to-first-rendered-buffer {(first_sound - note_on) / 1e6:.2f} ms; "
          f"max RS485 round trip {max(command_times):.2f} ms")
    for line in (work / "audio.log").read_text().splitlines():
        if line.startswith(("sim: PortAudio", "audio-test:")):
            print(line)
    played = re.search(r"device requested (\d+) frames", (work / "audio.log").read_text())
    assert played and int(played[1]) > 48000, "device did not request samples"
    assert "voice underruns 0; dropped BODY blocks 0" in (work / "audio.log").read_text()


def check_live_envelope(blocks, full_rms):
    """Measure the 50/250 ms envelope from actual submitted sample PCM."""
    pcm = [sample / 2147483648 for _, block in blocks for sample in block]
    first = next(i for i, sample in enumerate(pcm) if sample != 0)
    last = next(i for i in range(len(pcm) - 1, -1, -1) if pcm[i] != 0)
    # Skip first nonzero's partial cycle. Windowed RMS is gain at roughly the
    # midpoint of each 1 ms sine cycle; 1.5% allows that alignment and int8 PCM.
    def gain_at(index):
        return math.sqrt(sum(s * s for s in pcm[index:index + 48]) / 48) / full_rms
    for ms in (5, 15, 25, 35, 45):
        gain = gain_at(first + ms * 48)
        expected = (ms + 0.5) / 50
        assert abs(gain - expected) < 0.015, ("attack", ms, gain, expected)
    assert abs(gain_at(first + 55 * 48) - 1) < 0.005, "attack did not reach sustain"
    # Work backwards from the final nonzero sample to measure release rather
    # than conflating serial/callback/queue latency with envelope duration.
    for remaining_ms in (25, 75, 125, 175, 225):
        gain = gain_at(last - remaining_ms * 48)
        expected = (remaining_ms - 0.5) / 250
        assert abs(gain - expected) < 0.01, ("release", remaining_ms, gain, expected)
    assert abs(gain_at(last - 260 * 48) - 1) < 0.005, "release did not begin from sustain"
    print("PASS native envelope PCM: 50 ms attack, steady sustain, 250 ms release to silence")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=pathlib.Path,
                        help="Captured pre-refactor channel_card source directory")
    parser.add_argument("--audio", action="store_true",
                        help="Also play and verify a quiet tone on the real macOS default output")
    args = parser.parse_args()
    if args.audio and sys.platform != "darwin":
        parser.error("--audio requires macOS")
    check_boundaries()
    with tempfile.TemporaryDirectory(prefix="channel-tests-") as directory:
        work = pathlib.Path(directory)
        compiler = work / "berry"
        run(["make", "-C", REPO / "berry_compiler", "-j4",
             "BUILD_DIR=" + str(work / "compiler"), "OUTPUT=" + str(compiler)])
        program = work / "envelope.bec"
        run([compiler, REPO / "berry_compiler/examples/channel_envelope.be", "-o", program])
        trace = build(ROOT, work / "current", "application_trace")
        envelope_trace = build(ROOT, work / "current", "envelope_test")
        expected_envelope = run([envelope_trace, program, 1])
        for size in (7, 48, 96, 511, 512, 1024):
            assert run([envelope_trace, program, size]) == expected_envelope, size
        print("PASS envelopes: attack/sustain/release, early/repeated note-off, retrigger, "
              "rapid replacement, clear, eight independent voices; identical PCM across buffer sizes")
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
        if sys.platform == "darwin":
            timing = work / "simulator_timing_test"
            run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I" + str(ROOT / "app"), "-I" + str(ROOT / "sim"),
                 TESTS / "simulator_timing_test.cpp", "-o", timing])
            run([timing])
            print("PASS simulator scheduling: policy values and startup failure paths")
            audio = work / "audio_device_test"
            run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I" + str(ROOT / "app"), "-I" + str(ROOT / "sim"),
                 *portaudio_flags()[0], TESTS / "audio_device_test.cpp", "-pthread", "-o", audio])
            run([audio])
            print("PASS audio backend: requested frames, stereo/scaling, lifecycle/errors")
        pty_check(work)
        if args.audio:
            audio_check(work, compiler)
            audio_check(work, compiler, envelope=True)
        for name in ("protocol_test", "boundary_test", "dac_trace"):
            exe = build(ROOT, work / "current", name)
            output = run([exe, program])
            if name in ("dac_trace", "protocol_test") and args.baseline:
                old = build(args.baseline.resolve(), work / "baseline", name, True)
                assert output == run([old, program]), "Pre-refactor " + name + " mismatch"
            print("PASS", name)


if __name__ == "__main__":
    main()
