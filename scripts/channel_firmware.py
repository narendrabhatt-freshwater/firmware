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

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1] / "channel_card"
BINARY = ROOT / "bin/channel_MCU.bin"
BUILD = ROOT / "build/release"

# ---- show the last successful build -----------------------------------------

def show_status():
    report = BUILD / "channel_MCU.info"
    built_binary = BUILD / "channel_MCU.bin"
    if not report.is_file() or not BINARY.is_file():
        raise ValueError("No firmware report available. Run make in app/ first.")
    if not built_binary.is_file() or BINARY.read_bytes() != built_binary.read_bytes():
        raise ValueError("Output binary differs from the last build. Run make in app/ first.")
    print(report.read_text(), end="")


# ---- find stm32cubeprogrammer -----------------------------------------------

def find_programmer():
    override = os.environ.get("CUBE_PROGRAMMER")
    if override:
        return shutil.which(override)
    found = shutil.which("STM32_Programmer_CLI")
    if found:
        return found
    app = pathlib.Path("/Applications/STMicroelectronics/STM32Cube/"
                       "STM32CubeProgrammer/STM32CubeProgrammer.app/Contents")
    candidates = [app / folder / "bin/STM32_Programmer_CLI"
                  for folder in ("Resources", "MacOs")]
    candidates += [pathlib.Path.home() / "STMicroelectronics/STM32Cube/"
                   "STM32CubeProgrammer/bin/STM32_Programmer_CLI",
                   pathlib.Path("/opt/ST/STM32CubeProgrammer/bin/STM32_Programmer_CLI")]
    candidates += sorted(pathlib.Path("/opt/ST").glob(
        "STM32CubeCLT_*/STM32CubeProgrammer/bin/STM32_Programmer_CLI"))
    return next((str(path) for path in candidates
                 if path.is_file() and os.access(path, os.X_OK)), None)


# ---- wait for the usb bootloader and flash ----------------------------------

def flash_firmware(port, check):
    if port and not re.fullmatch(r"USB[1-9][0-9]*", port):
        raise ValueError("Use --port USB1 (or USB2, etc.).")
    programmer = find_programmer()
    if not programmer:
        raise ValueError("STM32CubeProgrammer CLI not found.\n"
                         "Install STM32CubeProgrammer: "
                         "https://www.st.com/en/development-tools/stm32cubeprog.html\n"
                         "Then put STM32_Programmer_CLI on PATH or set CUBE_PROGRAMMER "
                         "to its executable path.")
    if not BINARY.is_file() or not BINARY.stat().st_size or not os.access(BINARY, os.R_OK):
        raise ValueError(f"Firmware missing, empty, or unreadable: {BINARY}. "
                         "Run make in app/ first.")
    print(f"Programmer: {programmer}\nFirmware:   {BINARY}", flush=True)
    if check:
        print("Programmer and firmware found. No board accessed.")
        return
    selected = f" ({port})" if port else ""
    print(f"Waiting for USB bootloader{selected}. Connect USB, move BOOT up, "
          "then press reset.\nPress Ctrl+C to cancel the wait.", flush=True)
    while True:
        listing = subprocess.run([programmer, "-l", "usb"], stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True)
        if listing.returncode:
            raise ValueError(listing.stdout + "\nCould not list USB bootloaders. "
                             "Check the USB connection and host USB permissions.")
        ports = sorted(set(re.findall(r"USB[0-9]+", listing.stdout)))
        if ports:
            if not port:
                if len(ports) != 1:
                    raise ValueError(f"Multiple USB bootloaders found ({' '.join(ports)}). "
                                     "Select the Channel Card with --port USBn.")
                port = ports[0]
                break
            if port in ports:
                break
        elif not re.search(r"No STM32 device in DFU|No DFU", listing.stdout, re.I):
            raise ValueError(listing.stdout + "\nCould not identify USB ports in programmer output.")
        time.sleep(1)
    print(f"Writing {BINARY} at 0x08000000 on {port} and verifying...", flush=True)
    result = subprocess.run([programmer, "-c", f"port={port}", "-w", str(BINARY),
                             "0x08000000", "-v"])
    if result.returncode:
        raise ValueError("Programming or verification failed. See the programmer output above.")
    print("Flash and verification complete. Move BOOT down, then press reset to run.")


# ---- command line -----------------------------------------------------------

def main():
    global ROOT, BUILD, BINARY
    parser = argparse.ArgumentParser(description="Local Channel Card status and USB flashing.")
    parser.add_argument("--card-root", type=pathlib.Path, default=ROOT,
                        help="firmware directory (defaults to this repository's channel_card)")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status", help="show the last build report without compiling or accessing hardware")
    flash = commands.add_parser("flash", help="wait for USB DFU, write firmware, and verify it")
    flash.add_argument("--port", help="select USB1, USB2, etc.")
    flash.add_argument("--check", action="store_true", help="check tools and binary without accessing hardware")
    args = vars(parser.parse_args())
    ROOT = args.pop("card_root").resolve()
    BUILD = ROOT / "build/release"
    BINARY = ROOT / "bin/channel_MCU.bin"
    command = args.pop("command")
    if command == "status":
        show_status()
    elif command == "flash":
        flash_firmware(**args)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nCancelled.", file=sys.stderr)
        sys.exit(130)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)
