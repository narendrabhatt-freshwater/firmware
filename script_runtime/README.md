# Shared Channel Berry runtime

The Channel Card runs one stripped Berry VM with eight independently loaded,
RAM-only program objects. Each object supplies three runtime handlers and may
also supply compile-time `on_init`; each voice owns 16 fixed native float state slots.

Programs provide these event handlers:

```berry
# optional: def on_init()
def on_note_on(key)
def on_note_off(has_pending)
def on_ramp_end()
```

When `on_init()` is omitted, the compiler embeds identity mapping and standard
C4 = 261.625565 Hz tuning. Optional init may call
`tuning_set(reference_key, reference_frequency)` once and configure the map with
`keymap_set(input_key, output_key)` or `keymap_fill(output_key)`. Runtime `key`
is the physical MIDI key and `has_pending` reports whether a replacement waits.

```text
input(id)
state_get(slot)
state_set(slot, value)
set_amplitude(value)
ramp(target, slope)
hold()
start_note()
discard_pending()
note_end()
led(red, green, blue, brightness)
keymap_get(key)
pow(base, exponent)
```

The lower-level `input(id)` call remains available for less-common data. Input
constants are `INPUT_NOTE_ID`, `INPUT_FREQUENCY`, `INPUT_GAIN`, `INPUT_GATE`,
`INPUT_ACTIVE`, `INPUT_HAS_PENDING`, `INPUT_PENDING_FREQUENCY`,
`INPUT_PENDING_GAIN`, `INPUT_AMPLITUDE`, `INPUT_KEY`,
`INPUT_MAPPED_KEY`, `INPUT_PENDING_KEY`, and `INPUT_PENDING_MAPPED_KEY`.
Scripts implement pitch-dependent envelope policy themselves with allocation-free
`pow()` and a two-argument `ramp()`.

`start_note()` atomically promotes a transport-ready pending generation;
`discard_pending()` rejects it without touching the current voice, and
`note_end()` ends only the current voice. Crash/retrigger timing belongs entirely
to the script.

`led()` controls the Channel Card's RGB package. All four arguments are
normalized from `0.0` to `1.0`; brightness scales the selected color. The most
recent call from any voice owns the card-wide LED until reset.

Persistent per-voice state can use names instead of numeric slots. Declare a
name exactly once inside any handler with `state name` or initialize it at that
point with `state name = expression`. Declarations and assignments are complete
line statements. Reads in expressions and assignments in every handler then
use the name directly:

```berry
def on_note_on(key)
    state stage = 1
end

def on_note_off(has_pending)
    stage = 3
end
```

The compiler lowers these operations to `state_get` and `state_set`. A program
may declare at most 16 names; duplicate declarations and excess names are
compile-time errors. Named state cannot be mixed with direct numeric
`state_get`/`state_set` calls. Ordinary Berry `var` declarations remain
handler-local.

The target runtime has no source compiler, filesystem, REPL, bytecode saver, or
optional Berry modules. It uses float32/int32, a 25 KiB fixed arena including a
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

`fw_scriptc` emits an FWSC v1 container carrying ABI6 tuning/map metadata and bytecode:

```sh
build/shared-berry/fw_scriptc script_runtime/examples/channel_envelope.be \
  -o build/shared-berry/channel_envelope.fwsc
```
