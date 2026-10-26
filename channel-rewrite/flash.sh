#!/bin/sh
set -eu
cd "$(dirname "$0")"
programmer=${CUBE_PROGRAMMER:-STM32_Programmer_CLI}
if [ -z "${CUBE_PROGRAMMER:-}" ] && ! command -v "$programmer" >/dev/null 2>&1; then
    programmer=/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI
fi
target=$("$programmer" -q -l usb | awk -v requested="${2:-}" '
    /Device Index/ { port = $NF; serial = "" }
    /Serial number/ { serial = $NF }
    /Device ID/ && ($NF == "0x0483" || $NF == "0x483") && serial != "" {
        if (requested == "" || requested == port) {
            selected = port " " serial;
            ++count;
        }
    }
    END {
        if (count != 1) {
            print "Expected one H72x/H73x DFU target; connect the channel card or specify its USB port." > "/dev/stderr";
            exit 1;
        }
        print selected;
    }')
read -r port serial <<EOF
$target
EOF
printf 'Flashing channel target %s, serial %s\n' "$port" "$serial"
"$programmer" -c "port=$port" "sn=$serial" -w "${1:-bin/channel.bin}" 0x08000000 -v
printf '%s\n' 'Flash verified. Move BOOT down, then press reset to run.'
