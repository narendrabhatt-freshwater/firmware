#!/bin/sh
set -eu
cd "$(dirname "$0")"
programmer=${CUBE_PROGRAMMER:-STM32_Programmer_CLI}
if [ -z "${CUBE_PROGRAMMER:-}" ] && ! command -v "$programmer" >/dev/null 2>&1; then
    programmer=/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI
fi
"$programmer" -c "port=${2:-USB1}" -w "${1:-bin/channel.bin}" 0x08000000 -v
printf '%s\n' 'Flash verified. Move BOOT down, then press reset to run.'
