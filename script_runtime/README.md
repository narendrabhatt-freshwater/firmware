# Shared Channel Berry runtime

The Channel Card runs one stripped Berry VM with eight independently loaded,
RAM-only program objects. Each object supplies `on_note_on`, `on_note_off`, and
`on_ramp_end`; each voice also owns 16 fixed native float state slots.

Programs may call only:

```text
input(id)
state_get(slot)
state_set(slot, value)
set_amplitude(value)
ramp(target, slope)
hold()
start_note()
note_end()
```

Input constants are `INPUT_NOTE_ID`, `INPUT_FREQUENCY`, `INPUT_GAIN`,
`INPUT_GATE`, `INPUT_ACTIVE`, `INPUT_HAS_PENDING`,
`INPUT_PENDING_FREQUENCY`, `INPUT_PENDING_GAIN`, `INPUT_AMPLITUDE`, and
`INPUT_CRASH_RELEASE`.

The target runtime has no source compiler, filesystem, REPL, bytecode saver, or
optional Berry modules. It uses float32/int32, a 23 KiB fixed arena including a
4 KiB upload scratch region, pre-grown stacks, and a 32-instruction watchdog
heartbeat. Allocation or GC during a handler invalidates the shared VM.

Build and qualify on the host:

```sh
cmake -S script_runtime -B build/shared-berry -DCMAKE_BUILD_TYPE=Release
cmake --build build/shared-berry -j
ctest --test-dir build/shared-berry --output-on-failure
```

The qualification test loads eight distinct objects, performs replacement,
runs one million event dispatches, checks the fault-containment matrix, and
prints the allocator peak and formula-derived arena size.

`fw_scriptc` emits an FWSC v1 container carrying Channel Berry ABI3 bytecode:

```sh
build/shared-berry/fw_scriptc script_runtime/examples/channel_envelope.be \
  -o build/shared-berry/channel_envelope.fwsc
```
