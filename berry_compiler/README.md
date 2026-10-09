# Berry compiler and Channel scripting

This compiler is bundled with its matching Channel Card firmware. It also
builds independently from this folder, without an STM32 toolchain or downloads.
Use the compiler from the same release as the target firmware.

## Build and use

You need GNU Make and a C compiler (Clang or GCC). On macOS these are included
in the Xcode Command Line Tools. On Windows, use an MSYS2 MinGW environment
with Make and GCC installed, and run these commands in its shell.

From this directory, run:

```sh
make
./berry examples/channel_envelope.be -o channel_envelope.bec
```

On Windows the executable is `berry.exe`. Replace the example path with your
own `.be` script. The `.bec` output includes the firmware upload header,
ABI version, and checksum, ready to upload to the Channel Card.

To cross-compile for Linux ARM64, install Zig (`brew install zig` on macOS), then run:

```sh
make linux
```

This produces the static Linux executable `berry.linux-arm64`. Plain `make` builds for the current machine; each
target has its own object directory, so switching targets needs no clean.

`make clean` removes the Make build and executable. CMake is also supported:

```sh
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake --target berry --parallel
```

## Included Linux ARM64 executable

Linux ARM64 users can compile without installing a C toolchain:

```sh
./berry.linux-arm64 examples/channel_envelope.be -o channel_envelope.bec
```

Keep the executable permission when copying it. This binary runs on Linux
ARM64, not macOS or x86 Linux. Rebuild it with `make linux` when compiler
sources or the firmware ABI change; use the native `make` build on other hosts.

## Compiler sources

`berry.c` is the entry point; `vendor/berry/` contains the interpreter snapshot,
generated tables, license, and provenance. `config/` holds the host configuration;
`shared/` contains the source transformer and firmware container/ABI definitions.
The matching firmware copies live in `berry_runtime/`. The configurations differ
because the host compiles source while the card only loads bytecode.

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
| Numeric representation | Signed 32-bit integers and 32-bit floating point |
| MIDI keys | 128 values, numbered `0..127` |
| Source parser nesting | 25 nested parser levels on the host compiler |
| Berry heap / upload scratch | 80 KiB / 16 KiB in the explicitly placed VM arena |

Comments and whitespace in the `.be` source do not consume card storage, but
executable code, constants, and handler structures
increase the serialized bytecode size. The compiler rejects payloads exceeding
the 16 KiB container limit. Instruction and CPU-cycle counts are diagnostic
metrics in this revision, not enforced execution budgets. Keep handlers bounded
and short; they run in the audio path.

There is no separate supported count for handler-local `var` declarations.
They share the 128-value runtime stack with handler parameters, expression
temporaries, and native-call overhead. Persistent values have the clear API
limit: at most 64 named `state` values, or 64 numeric state slots, per voice
program. Keep locals to the few scalar values needed by the current handler.

## Loading a program

The compiler creates an FWSC `.bec` container; it does not communicate with the
card. Use a host implementing Channel USB protocol 2 to send HELLO, then
UPLOAD_BEGIN with kind 3, target voice 0..7, and the complete container size.
Send UPLOAD_DATA chunks with consecutive offsets and wait for each reply. The
final success reply confirms validation and installation. Upload only while
all voices are idle. Load each voice slot independently; programs are lost
on a card reset. Query them with RS485 `vm`, `vm <voice>`, or `vm mem`.

The Channel package's `docs/protocol.md` gives byte layouts and error handling.
Programs must use ABI2 and provide all three handlers below.

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

`on_note_on(key, velocity)` runs when a note is pending. An idle voice waits
for 998 BODY samples; an already active voice dispatches immediately so the
script can fade the outgoing note while replacement data arrives. `key`
is the physical MIDI key from `0` through `127`; `velocity` is the raw MIDI
velocity from `1` through `127`. The handler must call `start_note()` or `start_note(frequency)` to promote that pending note, or `discard_pending()` to
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
| `start_note(frequency)` | Starts the pending note at the script-supplied positive finite frequency in Hz. Omitting the argument looks up standard pitch for the pending key at activation. |
| `discard_pending()` | Removes the pending note without changing the current note; primarily used when `on_note_on()` rejects a transport-ready note. |
| `osc(wave, frequency)` | Appends a pending-note oscillator using logical wavetable `0..7` at an absolute frequency greater than 0 and no greater than 24,000 Hz, and returns an opaque note-local handle. |
| `route(source, OUTPUT, weight)` | Sends a pending oscillator's audio to the voice output with a nonnegative finite mix weight. |
| `modulate(source, target, control, amount)` | Uses a pending oscillator to modulate `FREQUENCY` or `AMPLITUDE` on `SAMPLE` or another oscillator. |
| `pitch_for_key(key)` | Returns the standard MIDI frequency for key `0..127` (A4 = 440 Hz). Using it is optional. |
| `note_end()` | Retires the current note and releases its playback resources. It does not promote a pending note. |
| `pow(base, exponent)` | Returns an allocation-free floating-point power calculation. |
| `led(red, green, blue, brightness)` | Sets the card RGB LED until the next `led()` call. Every argument is `0.0..1.0`; use `led(0, 0, 0, 0)` to turn it off. |
| `state_get(slot)` | Returns persistent state slot `0..63`. Prefer named state. |
| `state_set(slot, value)` | Stores a finite number in persistent state slot `0..63`. Prefer named state. |

Native functions other than `input()`, `pitch_for_key()`, `pow()`, `osc()`,
and `state_get()` return no value. Invalid argument counts, types, ranges, or
non-finite results fault the voice.

Pending notes carry key and velocity without a calculated pitch.
`start_note()` selects standard MIDI pitch at activation; an explicit frequency
selects a different tuning:

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
Upload them using binary UPLOAD_BEGIN kind 2 and offset-checked UPLOAD_DATA
chunks. The host supplies logical wave `0..7`; firmware owns physical placement.

The pending oscillator declaration is cleared before every `on_note_on`.
Calling `osc()` returns a positive opaque handle, which may be assigned or
ignored. Handles are exactly representable in persistent float32 state.
They survive pending promotion but become invalid on discard,
note end, replacement, fault, or panic. The declaration and zeroed phases
become active when `start_note(frequency)` promotes the pending note. An older note
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
    start_note(pitch_for_key(key))
end
```

An oscillator with no explicit connection keeps its legacy, full-weight
connection to the output mixer. Its first successful `route()` or `modulate()`
removes that implicit connection; add `route(handle, OUTPUT, weight)` when it
should also remain audible. The sample has mixer weight 1, implicit oscillator
routes have weight 1, and explicit audio routes use their nonnegative finite
weight.
The weighted sum is divided by the total weight before the existing filter,
common envelope, and voice gain.

`modulate(source, target, FREQUENCY, amount)` accepts `SAMPLE` or an oscillator
target. Its signed finite amount is peak deviation in Hz, applied as
`base + source * amount`. Instantaneous oscillator frequency is clamped to
0…24 kHz and sample playback to its existing 1/16×…16× root-pitch range.
`modulate(source, target, AMPLITUDE, gain)` accepts the same targets and a gain
from 0…1. It converts the bipolar oscillator source to unipolar and applies
`target *= clamp((source + 1) / 2, 0, 1) * gain`. Gain 1 therefore moves the
target between silence and its full level; gain 0.5 moves it between silence
and half level. Multiple amplitude modulators multiply.
Connections and handles must belong to the pending
note; duplicates, self-routes, and cycles fault the
voice. The complete graph becomes active at `start_note(frequency)` and follows the
note's normal discard, replacement, end, fault, and panic lifecycle.

```berry
def on_note_on(key, velocity)
    var modulator = osc(0, 7000)
    var carrier = osc(1, pitch_for_key(key))
    modulate(modulator, carrier, FREQUENCY, 250) # +/-250 Hz
    route(carrier, OUTPUT, 0.5)
    start_note(pitch_for_key(key))
end
```

Routing targets and controls are typed. Only the destinations and parameters
listed above are supported.

## Inputs

| Constant | Value returned by `input()` |
| --- | --- |
| `INPUT_NOTE_ID` | Channel Card voice index, `0..7`. |
| `INPUT_FREQUENCY` | Current note frequency in Hz, or pending frequency when no note is active. |
| `INPUT_GAIN` | Current fixed per-voice mixer gain, or pending gain when no note is active, normalized to `0.0..1.0`. |
| `INPUT_GATE` | `1` while the host currently requests the voice on; otherwise `0`. |
| `INPUT_ACTIVE` | `1` while a current note owns playback resources; otherwise `0`. |
| `INPUT_HAS_PENDING` | `1` when a transport-ready replacement note is waiting; otherwise `0`. |
| `INPUT_PENDING_FREQUENCY` | Zero before activation; store deferred explicit pitch in named state. |
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
- Each compiled program payload is limited to 16 KiB. Instruction and cycle
  counts are recorded, but there is no instruction or CPU-cycle watchdog in
  this revision.
- The eight voice programs share one 96 KiB fixed-arena reserve, including the
  16 KiB upload scratch. A bad native call
  or ordinary exception silences the affected voice. Allocation, garbage
  collection or interpreter-integrity faults invalidate all eight
  programs.
- Upload is accepted only while the Channel Card is idle.

A complete attack/release example is included in
[`examples/channel_envelope.be`](examples/channel_envelope.be).

Firmware queues key/velocity without a default pitch. `start_note(frequency)`
uses script-supplied Hz; `start_note()` looks up the pending key at activation.
`INPUT_PENDING_FREQUENCY` is zero before activation. Store explicit pitch in
named state if activation is deferred to `on_ramp_end()`.
