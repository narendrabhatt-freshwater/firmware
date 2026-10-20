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

"""Build isolated application tests using shared serial and audio buffers."""
import hashlib
import pathlib
import re
import subprocess
import tempfile

TESTS = pathlib.Path(__file__).resolve().parent
REPO = TESTS.parents[2]
ROOT = REPO


def run(args, **kwargs):
    result = subprocess.run([str(a) for a in args], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, **kwargs)
    if result.returncode:
        raise RuntimeError(result.stdout.decode(errors="replace") +
                           result.stderr.decode(errors="replace"))
    return result.stdout


def application_sources(root):
    makefile = (root / "channel_card/app/Makefile").read_text().replace("\\\n", " ")
    match = re.search(r"^SHARED_APP_SOURCES\s*:=\s*(.*)$", makefile, re.M)
    assert match, "Missing shared application source list in app/Makefile"
    return match[1].split()


def build(root, work, main):
    sources = application_sources(root)
    sources += ["channel_card/app/audio.cpp", "channel_card/app/audio_dac.cpp"]
    sources += ["channel/berry_runtime/vm.c", "channel/berry_runtime/vm_source.c",
                "channel/berry_runtime/berry_backend.c", "channel/berry_runtime/berry_runtime_modtab.c"]
    sources += [str(p.relative_to(root)) for p in sorted(
        (root / "channel/berry_runtime/third_party/berry/src").glob("*.c"))]
    sources += ["tests/" + main + ".cpp", "tests/backend_model.cpp"]
    executable = work / main
    args = ["make", "-f", TESTS / "Makefile", "ROOT=" + str(root), "-j4", "OBJDIR=" + str(work / "obj"),
            "OUTPUT=" + str(executable), "SOURCES=" + " ".join(sources)]
    run(args)
    return executable


def check_boundaries():
    sources = application_sources(ROOT)
    forbidden = re.compile(r"\b(?:HAL_\w+|DWT|CoreDebug|__disable_irq|__get_PRIMASK|"
                           r"__set_PRIMASK|__DMB|__arm__|__thumb__)\b")
    for source in sources:
        text = (ROOT / source).read_text()
        text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
        assert not forbidden.search(text), f"Hardware leaked into {source}"
        assert not re.search(r'#include\s+["<](?:main|board|cs4304|board_stm32|usb_device)\.h', text)
    for source in ("board_stm32.cpp", "usb_device.cpp"):
        assert "process_usb_messages(" not in (ROOT / "channel_card/app" / source).read_text()
    print("PASS application/platform dependency boundaries")


def main():
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
        expected_envelope = run([envelope_trace, program, 48])
        for size in (96, 240, 480, 1008):
            assert run([envelope_trace, program, size]) == expected_envelope, size
        print("PASS envelopes: attack/sustain/release, early/repeated note-off, retrigger, "
              "rapid replacement, clear, eight independent voices; identical PCM across groups of fixed 48-sample calls")
        for scenario in ("envelope", "oscillator", "fault"):
            fixture = work / (scenario + ".bec")
            source = (REPO / "berry_compiler/examples/channel_envelope.be" if scenario == "envelope"
                      else TESTS / (scenario + ".be"))
            run([compiler, source, "-o", fixture])
            expected = run([trace, fixture, 480])
            for size in (48, 96, 240):
                actual = run([trace, fixture, size])
                assert actual == expected, f"{scenario} mismatch at partition {size}"
            print("PASS fixed-call audio/state:", scenario, hashlib.sha256(expected).hexdigest())
        for name in ("protocol_test", "boundary_test", "dac_trace"):
            exe = build(ROOT, work / "current", name)
            output = run([exe, program])
            print("PASS", name)


if __name__ == "__main__":
    main()
