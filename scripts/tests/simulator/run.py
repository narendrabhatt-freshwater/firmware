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

"""Check the standalone wrapper without requiring a sound device."""
import os
from pathlib import Path
import subprocess
import tempfile

TESTS = Path(__file__).resolve().parent
SIMULATOR = TESTS.parents[2] / "simulator"
APPLICATION = TESTS.parents[2] / "channel_card/app"
prefix = os.environ.get("PORTAUDIO_PREFIX")
if not prefix:
    prefix = next((p for p in ("/opt/local", "/opt/homebrew", "/usr/local")
                   if Path(p, "include/portaudio.h").exists()), None)
with tempfile.TemporaryDirectory(prefix="simulator-tests-") as directory:
    output = Path(directory) / "wrapper_test"
    command = [os.environ.get("HOST_CXX", "c++"), "-std=c++17", "-O2", "-g",
               "-Wall", "-Wextra", "-Werror", "-pthread", "-I" + str(SIMULATOR)]
    if prefix:
        command += ["-I" + str(Path(prefix) / "include")]
    command += [str(TESTS / "wrapper_test.cpp"), "-o", str(output)]
    subprocess.run(command, check=True)
    subprocess.run([output], check=True, timeout=10)
    subprocess.run(["make", "-s", "-C", str(SIMULATOR), "-j4"], check=True)
    flags = [os.environ.get("HOST_CXX", "c++"), "-std=c++17", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
             "-I" + str(TESTS / "stm32"), "-I" + str(APPLICATION.parent / "core/inc"), "-I" + str(APPLICATION), "-I" + str(TESTS.parents[2] / "channel")]
    for count in (2, 3, 10):
        config = Path(directory) / ("dma_config_" + str(count))
        config.mkdir()
        (config / "hardware.h").write_text(
            f"#define BUF_COUNT {count}\n#define AUDIO_BUFFER_FRAMES 48u\n#define AUDIO_BUFFER_CHANNELS 2u\n")
        output = Path(directory) / ("dma_test_" + str(count))
        subprocess.run(flags[:1] + ["-I" + str(config)] + flags[1:] + [
                                str(TESTS / "dma_test.cpp"),
                                "-o", str(output)], check=True)
        subprocess.run([output], check=True, timeout=10)
    serial_flags = [os.environ.get("HOST_CXX", "c++"), "-std=c++17", "-O2", "-g",
                    "-Wall", "-Wextra", "-Werror", "-I" + str(TESTS / "stm32_serial"),
                    "-I" + str(APPLICATION.parent / "core/inc"), "-I" + str(APPLICATION), "-I" + str(TESTS.parents[2] / "channel")]
    for name in ("rs485_test", "usb_driver_test"):
        output = Path(directory) / name
        subprocess.run(serial_flags + [str(TESTS / (name + ".cpp")), "-o", str(output)], check=True)
        subprocess.run([output], check=True, timeout=10)
    output = Path(directory) / "interrupt_test"
    subprocess.run(flags + [str(TESTS / "interrupt_test.cpp"), "-o", str(output)], check=True)
    subprocess.run([output], check=True, timeout=10)
print("PASS startup prefill, write-index wrap and slot reuse, 48-sample sine generation, 480-sample callbacks and continuity, "
      "callback return before refill, ten request tokens after copy, one refill per select pass, unconditional generation on serial wakes, consecutive callbacks, "
      "independent USB/RS485 wakes, retained serial byte order, raw PTYs, "
      "audio restart, SIGINT/SIGTERM termination; "
      "STM32 DMA playback with 2/3/10 blocks, sleep/IRQ handoff, RS485 deadlines and USB fault/backpressure")
