# Desktop-testable Channel Card script runtime

This standalone C11 project builds the shared runtime core and fixed-arena Berry
backend from `channel_card/Core/{Inc,Src}/script`, plus a Berry-to-FWSC compiler,
deterministic simulator, tests, and an
optional ARM footprint target. It does not connect the runtime to Channel Card
firmware or the audio/USB paths.

```sh
cmake -S channel_card/tests/script_runtime -B build/script-runtime -DCMAKE_BUILD_TYPE=Release
cmake --build build/script-runtime -j
ctest --test-dir build/script-runtime --output-on-failure

build/script-runtime/fw_scriptc \
  channel_card/tests/script_runtime/examples/baseline_8x3.be \
  -o build/script-runtime/baseline.fwsc
build/script-runtime/control_runtime_sim build/script-runtime/baseline.fwsc \
  --ticks 1000000
```

`control_runtime_sim` uses a virtual 1 kHz clock. `--realtime` optionally paces
ticks against the wall clock; its timing is diagnostic and is not evidence for
the STM32H725 deadline. `--events` accepts:

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
cmake -S channel_card/tests/script_runtime -B build/script-runtime-arm \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/channel_card/cmake/gcc-arm-none-eabi.cmake" \
  -DSCRIPT_RUNTIME_ARM_FOOTPRINT=ON
cmake --build build/script-runtime-arm --target script_runtime_footprint -j
```

This produces `script_runtime_footprint.o`, prints its section sizes, and writes
`script_runtime_footprint.map`. It is a sizing artifact only and does not enable
the subsystem in firmware.
