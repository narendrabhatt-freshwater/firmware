# Channel scripting

Channel programs define the amplitude envelope, retrigger behavior, key map,
tuning, and optional RGB feedback for each voice. Sources use a restricted,
allocation-free subset of Berry and are compiled before upload to the Channel
Card.

## Limits at a glance

| Resource | Limit |
| --- | --- |
| Channel voices | 8, numbered `0..7` |
| Programs | One independently loaded program per voice |
| Editable `.be` source size | No fixed byte limit; source remains on the host and must compile within the limits below |
| Serialized Berry bytecode | 3,960 bytes maximum |
| Program metadata | 136 bytes per program |
| FWSC payload | 4,096 bytes maximum, including bytecode and metadata |
| Complete FWSC container | 4,116 bytes maximum, including the 20-byte header |
| Persistent state | 16 float32 values per voice program |
| Runtime stack | 128 VM values, including arguments, locals, expression temporaries, and call overhead |
| Runtime handler budget | 64 VM instructions per handler invocation |
| Whole-boundary budget | 512 VM instructions across all eight voices |
| Whole-boundary CPU budget | 55,000 Channel Card CPU cycles |
| Numeric representation | Signed 32-bit integers and 32-bit floating point |
| MIDI keys and key-map entries | 128 values, numbered `0..127` |
| Source parser nesting | 25 nested parser levels on the host compiler |
| VM memory arena | 26 KiB total, including a 4 KiB upload scratch region |

The 3,960-byte bytecode limit is the 4,096-byte FWSC payload minus its 136
bytes of Channel metadata. Comments and whitespace in the `.be` source do not
consume card storage, but executable code, constants, and handler structures
increase the serialized bytecode size. `loadVoiceScript()` and
`loadVoiceScriptSource()` return `ErrorCode::VmError` when the compiled result
is too large.

There is no separate supported count for handler-local `var` declarations.
They share the 128-value runtime stack with handler parameters, expression
temporaries, and native-call overhead. Persistent values have the clear API
limit: at most 16 named `state` values, or 16 numeric state slots, per voice
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
    if (!core.loadVoiceScript(voice, "attack_2ms.be")) return 1;
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

def on_note_off(has_pending)
end

def on_ramp_end()
end
```

`on_note_on(key, velocity)` runs when a transport-ready note is pending. `key`
is the physical MIDI key from `0` through `127`; `velocity` is the raw MIDI
velocity from `1` through `127`. The handler must call `start_note()` to
promote that pending note, or `discard_pending()` to reject it. Velocity zero
is handled as note-off before dispatch.

`on_note_off(has_pending)` runs when the host releases the voice. The Boolean
argument reports whether a replacement note is waiting. A program normally
ramps the current amplitude to zero and calls `note_end()` when that ramp ends.

`on_ramp_end()` runs once after a `ramp()` reaches its target. Store the current
envelope stage in persistent state when different ramps require different next
actions.

An optional `on_init()` runs in the host compiler, not on the card. It may only
configure tuning and key mapping:

```berry
def on_init()
    tuning_set(69, 440.0)
    keymap_set(36, 48)
end
```

Without `on_init()`, the key map is an identity map and tuning uses
C4 = 261.625565 Hz.

## Runtime functions

| Function | Description |
| --- | --- |
| `input(id)` | Returns the selected live voice value as a number. |
| `set_amplitude(value)` | Immediately sets amplitude to `0.0..1.0` and cancels the current ramp. |
| `ramp(target, slope)` | Ramps to `target` in `0.0..1.0`. `slope` is a positive amplitude change per second. |
| `hold()` | Stops ramping and holds the current amplitude. It does not emit `on_ramp_end()`. |
| `start_note()` | Replaces the current note with the transport-ready pending note. Fails when no note is pending. |
| `discard_pending()` | Removes the pending note without changing the current note. |
| `note_end()` | Retires the current note and releases its playback resources. It does not promote a pending note. |
| `keymap_get(key)` | Returns the mapped key for a physical key in `0..127`. |
| `pow(base, exponent)` | Returns an allocation-free floating-point power calculation. |
| `led(red, green, blue, brightness)` | Flashes the card RGB LED for 100 ms. Every argument is `0.0..1.0`. |
| `state_get(slot)` | Returns persistent state slot `0..15`. Prefer named state. |
| `state_set(slot, value)` | Stores a finite number in persistent state slot `0..15`. Prefer named state. |

Native functions other than `input()`, `keymap_get()`, `pow()`, and
`state_get()` return no value. Invalid argument counts, types, ranges, or
non-finite results fault the voice.

## Compile-time functions

These functions are valid only inside `on_init()`:

| Function | Description |
| --- | --- |
| `tuning_set(reference_key, frequency_hz)` | Sets one key in `0..127` to a positive reference frequency. Call at most once. |
| `keymap_set(input_key, output_key)` | Changes one physical key mapping. Both keys are `0..127`. |
| `keymap_fill(output_key)` | Maps every physical key to one key in `0..127`. |
| `keymap_get(input_key)` | Reads the current compile-time mapping. |

Runtime functions cannot be called from `on_init()`.

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
| `INPUT_MAPPED_KEY` | Mapped key of the current note. |
| `INPUT_PENDING_KEY` | Physical MIDI key of the pending note. |
| `INPUT_PENDING_MAPPED_KEY` | Mapped key of the pending note. |
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

def on_note_off(has_pending)
    stage = 2
end
```

A program may declare up to 16 named states. A declaration or assignment must
occupy a complete line. Named state cannot be mixed with direct
`state_get()`/`state_set()` calls. Ordinary `var` values are local to one
handler invocation.

Persistent state accepts finite numeric values and stores them as float32.
Handler-local `var` values should be integers, real numbers, Booleans, or
`nil`. The `key` and `velocity` handler arguments are integers and
`has_pending` is a Boolean.
Strings, byte buffers, lists, maps, instances, closures, and other allocated
objects are not supported inside runtime handlers because handler allocation
invalidates the shared VM.

## Runtime restrictions

- Only `on_init`, `on_note_on`, `on_note_off`, and `on_ramp_end` definitions
  may appear at the top level. Global variables, imports, classes, and helper
  functions are rejected.
- The target provides no filesystem, REPL, source compiler, bytecode saver, or
  optional Berry modules.
- Handler code must not allocate memory or trigger garbage collection. Keep it
  to numeric expressions, local scalar values, state, conditionals, and native
  calls.
- Each compiled program payload is limited to 4 KiB and each handler is limited
  to 64 VM instructions.
- The eight voice programs share one 26 KiB fixed-arena VM. A bad native call
  or ordinary exception silences the affected voice. Allocation, garbage
  collection, watchdog, or interpreter-integrity faults invalidate all eight
  programs.
- Upload is accepted only while the Channel Card is idle.

Production examples are in
[`examples/vm/channel`](examples/vm/channel/README.md).
