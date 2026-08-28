# Desktop-testable Channel Card script runtime

Everything for this milestone is contained in this directory: the portable C11
runtime, fixed-arena Berry backend, vendored Berry source, compiler, simulator,
examples, tests, and ARM footprint probe.

The runtime is ready to be linked into Channel Card firmware, but it is not yet
wired into the board's timer/control task, parameter mapping, USB/CDC upload
path, or 48 kHz audio interpolation. The desktop simulator is the current way
to run and verify it.

## Build and test

```sh
cmake -S script_runtime -B build/script-runtime -DCMAKE_BUILD_TYPE=Release
cmake --build build/script-runtime -j
ctest --test-dir build/script-runtime --output-on-failure

build/script-runtime/fw_scriptc \
  script_runtime/examples/baseline_8x3.be \
  -o build/script-runtime/baseline.fwsc
build/script-runtime/control_runtime_sim build/script-runtime/baseline.fwsc \
  --ticks 1000000
```

## What the 1 kHz clock means

The script runs once every millisecond and produces one control value for each
configured voice/output lane. This is a control-rate clock, not the STM32 CPU
clock and not the 48 kHz audio sample clock. A future firmware integration will
interpolate each result across the next 48 audio samples.

The simulator advances this clock virtually, so a million ticks do not take a
thousand seconds. `--realtime` optionally paces ticks against wall time; its
timing is diagnostic and is not evidence for the STM32H725 deadline.

`--events` accepts:

```text
<tick> control <name> <value> [ramp_ticks]
<tick> gate <voice> <0|1>
<tick> trigger <voice>
```

Lines beginning with `#` are ignored. `--csv FILE` writes every published
output as `tick,voice,parameter,value`. The JSON summary on stdout includes the
deterministic hash, instruction counts, lifecycle allocations/GC, fixed-arena
usage, queue/fault/clamp counts, and host timing distribution.

For an ARM footprint probe, configure this directory using the project ARM
toolchain and build only the probe target:

```sh
cmake -S script_runtime -B build/script-runtime-arm \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/channel_card/cmake/gcc-arm-none-eabi.cmake" \
  -DSCRIPT_RUNTIME_ARM_FOOTPRINT=ON
cmake --build build/script-runtime-arm --target script_runtime_footprint -j
```

This produces `script_runtime_footprint.o`, prints its section sizes, and writes
`script_runtime_footprint.map`. It is a sizing artifact only and does not enable
the subsystem in firmware.
