#!/bin/sh
set -e
if [ "$#" -eq 0 ]; then
    cat <<'HELP'

USAGE

    ./series2patch.sh <attack> [level] [decay]

ARGUMENTS

    attack    Amplitude/second scaled by velocity; negative = magnitude, 0 = instant
    level     Hold amplitude clamped to 0.0–1.0 by firmware
    decay     Amplitude/second; negative = magnitude, 0 = instant

HELP
    exit 0
fi
platform=$(uname -s)
compiler=./berry
if [ "$platform" = Linux ]; then
    compiler=./berry.linux-arm64
fi

for name in attack level decay; do
    [ "$#" -gt 0 ] || break
    value=$(printf '%s\n' "$1" | sed 's/[\&|]/\\&/g')
    if [ "$platform" = Linux ]; then
        sed -i -E "s|^(    (var )?$name = ).*|\\1$value|" series2.be
    else
        sed -i '' -E "s|^(    (var )?$name = ).*|\\1$value|" series2.be
    fi
    shift
done

"$compiler" series2.be -o series2.bec
