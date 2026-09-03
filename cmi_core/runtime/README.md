# Shared Channel Berry runtime

The Channel Card runs one stripped Berry VM with eight independently loaded,
RAM-only program objects. Each object supplies three runtime handlers and owns
64 fixed native float state slots.

Programs provide these event handlers:

```berry
def on_note_on(key, velocity)
def on_note_off()
def on_ramp_end()
```

Runtime `key` is the physical MIDI key and `velocity` is the raw MIDI value
from 1 through 127. Firmware cancels any waiting replacement before
`on_note_off()` runs.

```text
input(id)
state_get(slot)
state_set(slot, value)
set_amplitude(value)
ramp(target, slope)
start_note([frequency])
osc(wave, frequency_hz)
discard_pending()
note_end()
led(red, green, blue, brightness)
pitch_for_key(key)
pow(base, exponent)
```

The lower-level `input(id)` call remains available for less-common data. Input
constants are `INPUT_NOTE_ID`, `INPUT_FREQUENCY`, `INPUT_GAIN`, `INPUT_GATE`,
`INPUT_ACTIVE`, `INPUT_HAS_PENDING`, `INPUT_PENDING_FREQUENCY`,
`INPUT_PENDING_GAIN`, `INPUT_AMPLITUDE`, `INPUT_KEY`, `INPUT_PENDING_KEY`,
`INPUT_VELOCITY`, and `INPUT_PENDING_VELOCITY`.
Scripts implement pitch-dependent envelope policy themselves with allocation-free
`pow()` and a two-argument `ramp()`.

`start_note()` atomically promotes a transport-ready pending generation;
`discard_pending()` lets `on_note_on()` reject it without touching the current
voice, and `note_end()` ends the current voice. Note-off automatically discards
any waiting replacement. Crash/retrigger timing belongs entirely to the script.

Every pending note initially uses standard MIDI pitch with A4 = 440 Hz.
`pitch_for_key(key)` exposes that frequency as an optional reference.
`start_note()` uses the default, while `start_note(frequency)`
atomically overrides it with any positive frequency in Hz and promotes the
pending note without requiring the lookup.

`osc(wave, frequency_hz)` appends a pending-note oscillator and returns a
positive opaque handle that scripts may assign or ignore. Logical wave IDs
`0..7` map to reserved attack-bank IDs `248..255`. Frequency must be finite,
positive, and no greater than 24 kHz. Declarations must precede `start_note()`;
handles remain valid through promotion and provide future modulation identity.
Berry and firmware expose no predefined oscillator count. Each call allocates
one firmware-side descriptor dynamically, independently of wavetable size, and
any number of oscillators may reference the same table until card RAM is
exhausted. Allocation failure is reported as a voice-local native-call fault.

`led()` sets the Channel Card's RGB package until the next `led()` call. All
four arguments are normalized from `0.0` to `1.0`; brightness scales the
selected color. Call `led(0, 0, 0, 0)` to turn it off. The most recent call from
any voice owns the card-wide LED.

Persistent per-voice state can use names instead of numeric slots. Declare a
name exactly once inside any handler with `state name` or initialize it at that
point with `state name = expression`. Declarations and assignments are complete
line statements. Reads in expressions and assignments in every handler then
use the name directly:

```berry
def on_note_on(key, velocity)
    state stage = 1
end

def on_note_off()
    stage = 3
end
```

The compiler lowers these operations to `state_get` and `state_set`. A program
may declare at most 64 names; duplicate declarations and excess names are
compile-time errors. Named state cannot be mixed with direct numeric
`state_get`/`state_set` calls. Ordinary Berry `var` declarations remain
handler-local.

The target runtime has no source compiler, filesystem, REPL, bytecode saver, or
optional Berry modules. It uses float32/int32, a 96 KiB fixed arena including a
16 KiB upload scratch region, pre-grown stacks, and a 32-instruction watchdog
heartbeat. Allocation or GC during a handler invalidates the shared VM.

Build and qualify on the host:

```sh
cmake -S cmi_core/runtime -B build/shared-berry -DCMAKE_BUILD_TYPE=Release
cmake --build build/shared-berry -j
ctest --test-dir build/shared-berry --output-on-failure
```

The qualification test loads eight distinct objects, performs replacement,
loads and repeatedly replaces eight generated programs larger than the former
4 KiB cap, runs one million event dispatches, checks the fault-containment
matrix, and verifies at least 20% heap headroom over the measured peak.

`fw_scriptc` emits an FWSC v1 container carrying ABI1 Berry bytecode:

```sh
build/shared-berry/fw_scriptc cmi_core/runtime/examples/channel_envelope.be \
  -o build/shared-berry/channel_envelope.fwsc
```
