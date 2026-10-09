#!/usr/bin/env bash
set -euo pipefail

# ---- locate the firmware ----------------------------------------------------

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY="$ROOT/bin/channel_MCU.bin"
PORT=""
CHECK=0

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --check) CHECK=1; shift ;;
        --port)
            [[ $# -ge 2 && "$2" =~ ^USB[1-9][0-9]*$ ]] || fail "Use --port USB1 (or USB2, etc.)."
            PORT="$2"; shift 2 ;;
        -h|--help)
            printf '%s\n' \
                'Usage: ./flash.sh [--port USB1] [--check]' \
                'Wait for USB DFU, flash bin/channel_MCU.bin, and verify it.' \
                'Build with make in app/ first. BOOT up, then reset; Ctrl+C cancels the wait.' \
                '--check checks the programmer and binary without accessing the board.' \
                'Set CUBE_PROGRAMMER to the CLI executable for a custom installation.'
            exit 0 ;;
        *) fail "Unknown option: $1. Use --help." ;;
    esac
done

# ---- find stm32cubeprogrammer -----------------------------------------------

resolve_programmer() {
    local candidate
    if [[ -n "${CUBE_PROGRAMMER:-}" ]]; then
        command -v "$CUBE_PROGRAMMER"
        return
    fi
    if command -v STM32_Programmer_CLI; then
        return
    fi
    for candidate in \
        /Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/{Resources,MacOs}/bin/STM32_Programmer_CLI \
        "$HOME/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI" \
        /opt/ST/STM32CubeProgrammer/bin/STM32_Programmer_CLI \
        /opt/ST/STM32CubeCLT_*/STM32CubeProgrammer/bin/STM32_Programmer_CLI
    do
        if [[ -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

PROGRAMMER="$(resolve_programmer)" || fail "STM32CubeProgrammer CLI not found.
Install STM32CubeProgrammer: https://www.st.com/en/development-tools/stm32cubeprog.html
Then put STM32_Programmer_CLI on PATH or set CUBE_PROGRAMMER to its executable path."
[[ -x "$PROGRAMMER" ]] || fail "Programmer is not executable: $PROGRAMMER"
[[ -s "$BINARY" && -r "$BINARY" ]] || fail "Firmware missing, empty, or unreadable: $BINARY. Run make in app/ first."
printf 'Programmer: %s\nFirmware:   %s\n' "$PROGRAMMER" "$BINARY"
if [[ "$CHECK" -eq 1 ]]; then
    printf 'Programmer and firmware found. No board accessed.\n'
    exit 0
fi

# ---- select the usb bootloader ----------------------------------------------

trap 'printf "\nCancelled.\n" >&2; exit 130' INT
printf 'Waiting for USB bootloader%s. Connect USB, move BOOT up, then press reset.\n' "${PORT:+ ($PORT)}"
printf 'Press Ctrl+C to cancel the wait.\n'
while :; do
    if ! listing="$("$PROGRAMMER" -l usb 2>&1)"; then
        printf '%s\n' "$listing" >&2
        fail "Could not list USB bootloaders. Check the USB connection and host USB permissions."
    fi
    ports=()
    while IFS= read -r port; do
        [[ -n "$port" ]] && ports+=("$port")
    done < <(printf '%s\n' "$listing" | awk '
        { while (match($0, /USB[0-9]+/)) {
            print substr($0, RSTART, RLENGTH); $0 = substr($0, RSTART + RLENGTH)
        }}' | sort -u)
    if [[ ${#ports[@]} -gt 0 ]]; then
        if [[ -z "$PORT" ]]; then
            break
        fi
        for port in "${ports[@]}"; do
            [[ "$port" != "$PORT" ]] || break 2
        done
    elif ! printf '%s\n' "$listing" | grep -qiE 'No STM32 device in DFU|No DFU'; then
        printf '%s\n' "$listing" >&2
        fail "Could not identify USB ports in programmer output."
    fi
    sleep 1
done
if [[ -z "$PORT" ]]; then
    [[ ${#ports[@]} -eq 1 ]] || fail "Multiple USB bootloaders found (${ports[*]}). Select the Channel Card with --port USBn."
    PORT="${ports[0]}"
fi

# ---- write and verify the firmware ------------------------------------------

printf 'Writing %s at 0x08000000 on %s and verifying...\n' "$BINARY" "$PORT"
if ! "$PROGRAMMER" -c "port=$PORT" -w "$BINARY" 0x08000000 -v; then
    fail "Programming or verification failed. See the programmer output above."
fi
printf 'Flash and verification complete. Move BOOT down, then press reset to run.\n'
