# Desktop-testable Channel Card script runtime

Everything for this milestone is contained in this directory: the portable C11
runtime, fixed-arena Berry backend, vendored Berry source, compiler, simulator,
examples, tests, and ARM footprint probe.

The runtime is ready to be linked into Channel Card firmware, but it is not yet
wired into the board's timer/control task, output mapping, USB/CDC upload
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
configured note/output lane. This is a control-rate clock, not the STM32 CPU
clock and not the 48 kHz audio sample clock. A future firmware integration will
interpolate each result across the next 48 audio samples.

The simulator advances this clock virtually, so a million ticks do not take a
thousand seconds. `--realtime` optionally paces ticks against wall time; its
timing is diagnostic and is not evidence for the STM32H725 deadline.

## Note terminology and script API

A `note` is one of the Channel Card's eight note slots (`0` through `7`, matching
`n0` through `n7`). Low-level DSP code may call the same processing slot a
voice, but the script, simulator, and runtime public APIs consistently call it
a note.

Berry programs can call only these Channel Card functions:

```text
configure_outputs(count)
define_control(name, min, max, default, slew_ms)
control_get(id)
note_is_on(note)
note_started(note)
output_set(note, output, value)
tick_index()
```

`note_on` makes the note active and makes `note_started(note)` true for that
tick. `note_restart` makes `note_started(note)` true for one tick without
changing the active state. `note_off` makes the note inactive.

This terminology change is runtime ABI version 2. Recompile older `.be` source
with the current `fw_scriptc`; ABI-1 `.fwsc` files are intentionally rejected.

`--events` accepts:

```text
<tick> control <name> <value> [ramp_ticks]
<tick> note_on <note>
<tick> note_off <note>
<tick> note_restart <note>
```

Lines beginning with `#` are ignored. `--csv FILE` writes every published
output as `tick,note,output,value`. The JSON summary on stdout includes the
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
