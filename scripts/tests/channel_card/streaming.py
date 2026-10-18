#!/usr/bin/env python3
"""Software-only integration using the production host streamer and PTY adapter.
Retains binaries/logs in --work-dir. --timeout explicitly enables a test watchdog;
it does not configure or change production transaction deadlines.
"""
import argparse
import os
import pathlib
import signal
import subprocess
import time
import run as tests

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--work-dir", type=pathlib.Path, required=True)
p.add_argument("--seconds", type=int, default=2)
p.add_argument("--program-source", type=pathlib.Path,
               default=tests.REPO / "berry_compiler/examples/channel_envelope.be",
               help="Berry envelope to exercise with the production host")
p.add_argument("--timeout", type=float)
p.add_argument("--tsan", action="store_true", help="Instrument both host and simulator threads")
p.add_argument("--audio-stall-ms", type=int, default=0,
               help="Diagnostic only: pause the fake audio thread once after 1 s")
p.add_argument("--source-root", type=pathlib.Path, default=tests.ROOT,
               help="Channel Card source snapshot to compare")
p.add_argument("--host-root", type=pathlib.Path, default=tests.REPO / "voice_bd",
               help="Host source snapshot to compare")
a = p.parse_args()
if a.audio_stall_ms < 0:
    p.error("--audio-stall-ms must be nonnegative")
tests.ROOT = a.source_root.resolve()
w = a.work_dir.resolve(); w.mkdir(parents=True, exist_ok=True)
compiler = w / "berry"
tests.run(["make", "-C", tests.REPO / "berry_compiler", "-j4",
           "BUILD_DIR=" + str(w / "compiler"), "OUTPUT=" + str(compiler)])
program = w / "envelope.bec"
tests.run([compiler, a.program_source.resolve(), "-o", program])
sources = tests.application_sources(tests.ROOT)
sources += ["berry_runtime/vm.c", "berry_runtime/berry_backend.c",
            "berry_runtime/berry_runtime_modtab.c"]
sources += [str(f.relative_to(tests.ROOT)) for f in sorted(
    (tests.ROOT / "berry_runtime/third_party/berry/src").glob("*.c"))]
sources += ["sim/serialdevices.cpp", "sim/clock.cpp",
            "tests/fake_portaudio.cpp", "tests/streaming_main.cpp"]
sanitize = " -fsanitize=thread" if a.tsan else ""
tests.run(["make", "-f", tests.TESTS / "Makefile", "-j4", "SOURCES=" + " ".join(sources),
           "ROOT=" + str(tests.ROOT), "OBJDIR=" + str(w / ("objects-tsan" if a.tsan else "objects")), "OUTPUT=" + str(w / "sim"),
           "COMMON_FLAGS=-O1 -g -Wall -MMD -MP" + sanitize,
           "EXTRA_INCLUDES=" + " ".join(tests.portaudio_flags()[0]), "LDLIBS=-lm -pthread" + sanitize])
flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "libserialport"], text=True).split()
tests.run(["c++", "-std=c++17", "-O2", "-g", "-pthread", "-I" + str(a.host_root.resolve()),
           tests.TESTS / "host_streaming_test.cpp", *flags,
           *(["-fsanitize=thread"] if a.tsan else []), "-o", w / "host"])
env = dict(os.environ, CHANNEL_TEST_DIRECTORY=str(w),
           CHANNEL_TEST_AUDIO_STALL_MS=str(a.audio_stall_ms))
with (w / "sim.log").open("wb") as log:
    sim = subprocess.Popen([w / "sim"], env=env, stdout=log, stderr=log)
    start = time.monotonic()
    try:
        while not (w / "rs485").exists():
            if sim.poll() is not None: raise RuntimeError((w / "sim.log").read_text())
            if a.timeout and time.monotonic() - start >= a.timeout:
                raise TimeoutError("explicit startup watchdog expired")
            time.sleep(0.001)
        result = subprocess.run([w / "host", program, w / "usb", w / "rs485", str(a.seconds)],
                                capture_output=True, text=True, timeout=a.timeout)
        (w / "host.log").write_text(result.stdout + result.stderr)
        print(result.stdout + result.stderr, end="", flush=True)
        if result.returncode: raise RuntimeError("Host streaming failed; evidence retained in " + str(w))
    finally:
        if sim.poll() is None: sim.send_signal(signal.SIGTERM)
        try: sim.wait(timeout=a.timeout)
        except subprocess.TimeoutExpired:
            sim.kill(); sim.wait(); raise
    if sim.returncode: raise RuntimeError("Simulator failed; see " + str(w / "sim.log"))
print("PASS production host streamer → PTYs → shared protocol → threaded audio consumer")
