# Channel scripting

Channel programs define pitch, amplitude envelope, retrigger behavior, and
optional RGB feedback for each voice. Sources use a restricted Berry subset
that does not create Berry heap objects while handlers run and are compiled
before upload to the Channel Card.

## Limits at a glance

| Resource | Limit |
| --- | --- |
| Channel voices | 8, numbered `0..7` |
| Programs | One independently loaded program per voice |
| Editable `.be` source size | No fixed byte limit; source remains on the host and must compile within the limits below |
| Serialized Berry bytecode / FWSC payload | 16,384 bytes maximum |
| Complete FWSC container | 16,404 bytes maximum, including the 20-byte header |
| Persistent state | 64 float32 values per voice program |
| Runtime stack | 128 VM values, including arguments, locals, expression temporaries, and call overhead |
| Runtime handler budget | 64 VM instructions per handler invocation |
| Whole-boundary budget | 512 VM instructions across all eight voices |
| Whole-boundary CPU budget | 55,000 Channel Card CPU cycles |
| Numeric representation | Signed 32-bit integers and 32-bit floating point |
| MIDI keys | 128 values, numbered `0..127` |
| Source parser nesting | 25 nested parser levels on the host compiler |
| Berry heap / upload scratch | 80 KiB / 16 KiB in the explicitly placed VM arena |

Comments and whitespace in the `.be` source do not consume card storage, but
executable code, constants, and handler structures
increase the serialized bytecode size. `loadVoiceScript()` and
`loadVoiceScriptSource()` return `ErrorCode::VmError` when the compiled result
is too large.

There is no separate supported count for handler-local `var` declarations.
They share the 128-value runtime stack with handler parameters, expression
temporaries, and native-call overhead. Persistent values have the clear API
limit: at most 64 named `state` values, or 64 numeric state slots, per voice
program. Keep locals to the few scalar values needed by the current handler.

## Loading a program

Connect first, then load each voice slot independently. Different voices may
use different programs:

```cpp
cmi::CoreParams params;
params.rs485_port = "/dev/cu.usbserial-0001";
params.channel_cdc_port = "/dev/cu.usbmodemCHCARD_0123456789ABCDEF012345673";
cmi::Core core(params);
if (!core.connect()) return 1;

for (uint8_t voice = 0; voice < 8; ++voice) {
    if (!core.loadVoiceScript(voice, "channel.be")) return 1;
}
```

`loadVoiceScriptSource(voice, source)` accepts source text instead of a file.
Direct note operations work as soon as their selected voice has a program;
automatic MIDI input starts after all eight slots are loaded. Programs reside
in Channel Card RAM and must be loaded again after a card reset.

## Program structure

Every program provides these runtime handlers:

```berry
def on_note_on(key, velocity)
end

def on_note_off()
end

def on_ramp_end()
end
```

`on_note_on(key, velocity)` runs when a transport-ready note is pending. `key`
is the physical MIDI key from `0` through `127`; `velocity` is the raw MIDI
velocity from `1` through `127`. The handler must call `start_note()` or
`start_note(frequency)` to promote that pending note, or `discard_pending()` to
reject it. Velocity zero is handled as note-off before dispatch.

`on_note_off()` runs when the host releases the voice. Firmware first cancels
any pending replacement and its buffered BODY, so release scripts only manage
the current note. A program normally ramps the current amplitude to zero and
calls `note_end()` when that ramp ends.

`on_ramp_end()` runs once after a `ramp()` reaches its target. Store the current
envelope stage in persistent state when different ramps require different next
actions.

## Runtime functions

| Function | Description |
| --- | --- |
| `input(id)` | Returns the selected live voice value as a number. |
| `set_amplitude(value)` | Immediately sets amplitude to `0.0..1.0` and cancels the current ramp. |
| `ramp(target, slope)` | Ramps to `target` in `0.0..1.0`. `slope` is a positive amplitude change per second. |
| `start_note()` | Starts the pending note with standard MIDI pitch. Fails when no note is pending. |
| `start_note(frequency)` | Overrides the pending pitch with a positive frequency in Hz and starts the note. |
| `discard_pending()` | Removes the pending note without changing the current note; primarily used when `on_note_on()` rejects a transport-ready note. |
| `osc(wave, frequency)` | Appends a pending-note oscillator using logical wavetable `0..7` at an absolute frequency greater than 0 and no greater than 24,000 Hz, and returns an opaque note-local handle. |
| `pitch_for_key(key)` | Returns the standard MIDI frequency for key `0..127` (A4 = 440 Hz). Using it is optional. |
| `note_end()` | Retires the current note and releases its playback resources. It does not promote a pending note. |
| `pow(base, exponent)` | Returns an allocation-free floating-point power calculation. |
| `led(red, green, blue, brightness)` | Sets the card RGB LED until the next `led()` call. Every argument is `0.0..1.0`; use `led(0, 0, 0, 0)` to turn it off. |
| `state_get(slot)` | Returns persistent state slot `0..63`. Prefer named state. |
| `state_set(slot, value)` | Stores a finite number in persistent state slot `0..63`. Prefer named state. |

Native functions other than `input()`, `pitch_for_key()`, `pow()`, `osc()`,
and `state_get()` return no value. Invalid argument counts, types, ranges, or
non-finite results fault the voice.

Each pending note starts with standard MIDI pitch. `on_note_on()` may replace
it before activation:

```berry
def on_note_on(key, velocity)
    start_note(pitch_for_key(key) * 0.5) # optional lookup, octave down
end
```

A script may ignore the lookup entirely and call, for example,
`start_note(440.0)`. The physical key remains event metadata; changing pitch
does not rewrite it.

## Wavetable oscillators

Each `osc()` call appends an independent oscillator to the pending note.
Logical wavetable IDs `0..7` map to the eight reserved attack-bank entries
`248..255`. A table may be reused by multiple oscillators at different
frequencies. Tables should contain periodic signed-int8 data with at least two
and at most 512 samples and are interpolated cyclically from last to first.
Upload them with `wl <logical-wave> <nbytes>` over Channel USB CDC. The host
supplies only logical wave `0..7`; Channel firmware owns physical placement.

The pending oscillator declaration is cleared before every `on_note_on`.
Calling `osc()` returns a positive opaque handle, which may be assigned or
ignored. Handles are exactly representable in persistent float32 state.
They survive pending promotion but become invalid on discard,
note end, replacement, fault, or panic. The declaration and zeroed phases
become active when `start_note()` promotes the pending note. An older note
keeps its own oscillators while a replacement waits for a voice-steal fade.
There is no predefined oscillator-count constant in the Berry ABI or the
firmware implementation. Each `osc()` call dynamically allocates one small
descriptor, so any number of instances may reuse the same wavetable until card
RAM is actually exhausted. Allocation failure faults only the calling voice
instead of silently dropping a requested oscillator.

```berry
def on_note_on(key, velocity)
    var fundamental = pitch_for_key(key)
    var carrier = osc(0, fundamental)
    osc(1, fundamental * 2) # ignoring the handle is valid
    start_note()
end
```

The sample and enabled oscillators have equal source gain and are averaged
before the existing per-voice filter and common envelope. Filter pitch
tracking continues to use the primary note frequency. There is no
per-oscillator gain, envelope, phase, or modulation control yet; returned
handles establish the identity those future calls will use.

## Inputs

| Constant | Value returned by `input()` |
| --- | --- |
| `INPUT_NOTE_ID` | Channel Card voice index, `0..7`. |
| `INPUT_FREQUENCY` | Current note frequency in Hz, or pending frequency when no note is active. |
| `INPUT_GAIN` | Current fixed per-voice mixer gain, or pending gain when no note is active, normalized to `0.0..1.0`. |
| `INPUT_GATE` | `1` while the host currently requests the voice on; otherwise `0`. |
| `INPUT_ACTIVE` | `1` while a current note owns playback resources; otherwise `0`. |
| `INPUT_HAS_PENDING` | `1` when a transport-ready replacement note is waiting; otherwise `0`. |
| `INPUT_PENDING_FREQUENCY` | Pending note frequency in Hz. |
| `INPUT_PENDING_GAIN` | Pending fixed per-voice mixer gain normalized to `0.0..1.0`. |
| `INPUT_AMPLITUDE` | Current envelope amplitude in `0.0..1.0`. |
| `INPUT_KEY` | Physical MIDI key of the current note. |
| `INPUT_PENDING_KEY` | Physical MIDI key of the pending note. |
| `INPUT_VELOCITY` | Raw MIDI velocity of the current note, `1..127`, or `0` when inactive. |
| `INPUT_PENDING_VELOCITY` | Raw MIDI velocity of the pending note, `1..127`, or `0` when absent. |

The audio path applies `INPUT_GAIN` separately from the scripted envelope.
Velocity does not alter gain automatically; scripts decide whether and how to
use the handler argument or velocity inputs.

## Persistent state

Use `state` for values that must survive between handlers. Declare each name
exactly once in the program:

```berry
def on_note_on(key, velocity)
    state stage = 1
end

def on_note_off()
    stage = 2
end
```

A program may declare up to 64 named states. A declaration or assignment must
occupy a complete line. Named state cannot be mixed with direct
`state_get()`/`state_set()` calls. Ordinary `var` values are local to one
handler invocation.

Persistent state accepts finite numeric values and stores them as float32.
Handler-local `var` values should be integers, real numbers, Booleans, or
`nil`. The `key` and `velocity` handler arguments are integers.
Strings, byte buffers, lists, maps, instances, closures, and other allocated
objects are not supported inside runtime handlers because handler allocation
invalidates the shared VM.

## Runtime restrictions

- Only `on_note_on`, `on_note_off`, and `on_ramp_end` definitions
  may appear at the top level. Global variables, imports, classes, and helper
  functions are rejected.
- The target provides no filesystem, REPL, source compiler, bytecode saver, or
  optional Berry modules.
- Handler code must not allocate Berry objects or trigger Berry garbage
  collection. The native `osc()` implementation allocates its firmware-side
  descriptor and reports runtime heap exhaustion as a voice-local fault. Keep it
  to numeric expressions, local scalar values, state, conditionals, and native
  calls.
- Each compiled program payload is limited to 16 KiB and each handler is limited
  to 64 VM instructions.
- The eight voice programs share one 96 KiB fixed-arena reserve, including the
  16 KiB upload scratch. A bad native call
  or ordinary exception silences the affected voice. Allocation, garbage
  collection, watchdog, or interpreter-integrity faults invalidate all eight
  programs.
- Upload is accepted only while the Channel Card is idle.

Production examples are in
[`examples/vm/channel`](examples/vm/channel/README.md).
