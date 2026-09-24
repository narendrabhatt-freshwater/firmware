# Freshwater card protocol

Host ↔ Channel Card and Effect Card controls use RS485 ASCII commands and replies.
Effect Card also accepts text commands on USB CDC. Channel Card USB is a single
binary CDC stream for samples and uploads (§4); it has no USB text console.

Baud on the RS485 UARTs is **921600 8N1**.

Type `h` (or `help` or `?`) on either card for the live one-line menu.
If this document and the firmware disagree, trust the firmware.

---

## 1. How a line works

1. You send one command line.
2. The card runs it.
3. The card sends one reply line (sometimes more on Effect I2C scan / ADC init).

Input is folded to lower case. Spaces separate arguments.

### Addressing (RS485 multi-drop)

| Prefix   | Meaning           |
| -------- | ----------------- |
| `c:`     | Channel Card only |
| `e:`     | Effect Card only  |
| `*:`     | Both (broadcast)  |
| *(none)* | Also broadcast    |

Examples:

```text
c:n0 on 69
e:s
*:h
n0 on 69
```

On the Effect Card USB CDC console the address prefix is optional.

### End of line

- **Host → card:** end the command with a single `\r`. Do not send `\r\n`
  for one command — both CR and LF are treated as end-of-line, so
  `\r\n` runs the line twice.
- **Card → host:** replies end with `\r\n`.

### Reply tags

| Path    | Reply shape                                                       |
| ------- | ----------------------------------------------------------------- |
| RS485   | `[C]` or `[E]` then the body (note the space after the bracket) |
| Effect USB CDC | body only, no tag                                                 |

Examples of bodies (tag omitted for clarity):

```text
ok
ok: s
err:syntax
err:range
err:unknown
```

Compact note / gain success is often just `ok`. Query commands usually
return `ok: …` with the value.

### Errors you will see

| Body                                    | Meaning                                       |
| --------------------------------------- | --------------------------------------------- |
| `err:syntax`                            | Could not parse the line                      |
| `err:range`                             | Number or slot out of allowed range (Channel) |
| `err:unknown`                           | No such command                               |
| `err:usb`                               | CDC-only command (`al`, `wl`, or `vmload`) sent on RS485 |
| `err:rxdrop`                            | Channel UART RX overrun between lines         |
| `err:no-program`                        | Channel voice has no active VM program        |
| `err:vm-busy`                           | Note/program operation conflicts with VM upload or active playback |
| `err: ar …` / `err: aw …` / `err: adc…` | Effect I2C / ADC failure                      |

---

## 2. Channel Card commands

Eight note slots: `n0` … `n7` (voices 0–7). Slot digits `8`–`f` parse
but reply `err:range`. All voices mix onto DAC channel 1.

Each voice is a **SAMPLE voice**: note-on plays the assigned attack head
from AXI RAM, then the USB BODY slots, through one on-card playhead
(pitch, filter, and VM-controlled amplitude). Host streams unpitched body; the
card rate-scales (`note_Hz / root_Hz`). A script may layer eight looped
attack-bank wavetable oscillators onto that source, but a playable voice still
requires sample BODY data.

At boot the card turns the analog bypass path on and sets CH1 DAC trim to
0 dB. Note commands do not touch gain or bypass.

### Help

| Command            | Reply                 |
| ------------------ | --------------------- |
| `h` / `help` / `?` | One-line command list |

### Notes

| Command                | Meaning                                                                           |
| ---------------------- | --------------------------------------------------------------------------------- |
| `n0`…`n7 on <key> [velocity]` | Start raw MIDI key 0…127 with velocity 1…127; omitted velocity defaults to 127. |
| `n0`…`n7 on <sample> <key> <velocity> @<session>` | Assign sample and arm streamed note together; one small ACK. |
| `n0`…`n7 on <key> <velocity> @<session>` | Streamed note-on; bind BODY session 0…254 before ACK.          |
| `n0`…`n7 off`            | Turn that slot off.                                                            |
| `clear` / `n off`      | Hard-stop all 8, discard queued playback data and reset Berry state at the next audio boundary. Loaded samples/programs and USB transport state are retained. |

Bare `n0`…`n7` is a syntax error.

Success reply for sets: `ok`.

Fractional Hz is intentional (e.g. `261.625565` for C4). Do not round
to integers if you care about equal temperament.

### Shared bank (248 sample attacks + 8 oscillator wavetables)

Eight voices (`n0`…`n7`) select sample heads `0..247` directly in note-on.
The bank holds **256** signed-int8 heads of up to **512** samples (~10.7 ms @
48 kHz); IDs `248..255` are reserved for logical oscillator wavetables
`0..7`. Upload is USB CDC only (§4). Contents survive while powered and are
lost on reset.

| Command             | Meaning                                                             |
| ------------------- | ------------------------------------------------------------------- |
| USB upload kind 1 | Load sample head 0…247 (1…512 signed-int8 bytes); §4 |
| USB upload kind 2 | Load logical oscillator wave 0…7 (2…512 signed-int8 bytes); §4 |
| `ar <id> <Hz>`      | Set head `<id>`'s root pitch (Hz > 0)                               |
| `a`                 | Loaded count + 256-bit hex mask (bit 0 = wave 0)                    |
| `vq`                | All-voice remaining time, sessions, and exact ring credit             |
| `usb`               | BODY counters: drop/hold/fill, RS-485 `vq`, rx/bytes/bad              |
| `usb 0`             | Clear those counters, then same reply                                 |

Replies: `ok: ar <id> <Hz>`, `ok: a <n> <64 hex>`.
RS485 `vq` returns the fixed 61-byte frame described below.

Playback pitch is on-card: `phase_inc = note_Hz / root_Hz`, 2-tap
linear interpolation. The attack plays to its committed length (not a hold-pad to
512). Body starts at `len − 32` with the same source index and
fraction as the attack. The host does not count body-FIFO consume
until that join. Every `nX on <key> [velocity]` command is a note-on, including key 0.

### Channel VM programs

| Command                    | Meaning                                                          |
| -------------------------- | ---------------------------------------------------------------- |
| USB upload kind 3 | Upload one FWSC ABI2 program to voice 0…7; see §4 |
| `vm`                       | Query the active-program voice mask                              |
| `vm <voice>`               | Query active state, ABI target/version, and fault for one voice  |
| `vm mem`                   | Shared-arena metrics followed by eight per-voice diagnostic lines |
| `cpuload [0\|1]`           | Query or enable LED_Y/PB9 DMA-refill duty-cycle probe              |

Script upload kind 3 accepts a complete 20…16404-byte FWSC container through
offset-checked binary blocks. The full transaction is specified in §4.

### VM-controlled amplitude envelope

Amplitude ramps and note lifecycle are controlled only by the uploaded per-voice
VM program. The firmware exposes no separate envelope-programming commands.
See [SCRIPTING.md](../channel_card/SCRIPTING.md) and script uploads below.

Each `osc(wave, frequency_hz)` call appends a pending-note oscillator and
returns an opaque handle. ABI2 `route(source, OUTPUT, weight)` sends oscillator
audio to the voice output. `modulate(source, target, control, amount)` controls
`FREQUENCY` or `AMPLITUDE` on `SAMPLE` or another oscillator. An oscillator's
first explicit connection removes
its legacy implicit mixer route. Output routes use a weighted average;
frequency gain is signed deviation in Hz. Amplitude gain 0…1 converts the
source to 0…1 and multiplies the target level; multiple amplitude modulators
multiply. Routes form an acyclic pending-note graph committed by
`start_note()` before the existing filter and envelope.

### CPU-load scope probe

`cpuload 1` assigns LED_Y/PB9 to the SPI1 DMA refill probe. The pin is low
from entry to each half/full callback until all 48 frames have been refilled,
and high for the remaining part of the 1 ms audio period. Therefore
`CPU load = low pulse width / 1 ms`. This includes USB callback work, pending
Berry event handlers, and all sample/oscillator/filter/envelope rendering.
`cpuload 0` disables the probe and restores normal fixed-LED behavior;
`cpuload` reports its current state.

### Digital low-pass filter

| Command            | Meaning                             |
| ------------------ | ----------------------------------- |
| `f`                | Dump f0…f7                          |
| `f <Hz> [q]`       | Set base cutoff (± q) on voices 0–7 |
| `f0`…`f7`          | Query one                           |
| `f0`…`f7 <Hz> [q]` | Set one                             |

Cutoff (base at C4 when pitch-track is used):

- Allowed **[20, 20000]** Hz
- **`0` or `20000`** = bypass (transparent)

Optional `q` is the DF4 shape parameter **g**, range **[0.5, 10]**,
default **1.0**. Higher q peaks more near the corner. Omit `q` to leave
the current value.

`f8`…`ff` are rejected (`err:range`).

### Filter pitch tracking

| Command                       | Meaning                    |
| ----------------------------- | -------------------------- |
| `fk` / `fk <k>`               | Dump / set k on voices 0–7 |
| `fk0`…`fk7` / `fk0`…`fk7 <k>` | Query / set one            |

`k` in **[0, 10]**, default **0**.

```text
fc = fbase * (note_Hz / C4)^k
```

`f0` sets `fbase` (the corner **at C4**). It is not the key frequency.
`k = 0` leaves cutoff fixed. `k = 1` doubles the corner one octave above C4.

### DAC gain

| Command       | Meaning                                         |
| ------------- | ----------------------------------------------- |
| `g <ch> <dB>` | CS4304 atten on channel **1..4**, dB **0..127** |

Reply: `ok`.

### Channel quick examples

```text
c:g 1 0
c:n0 on 69
c:f0 300
c:fk0 1
c:n0 on 72
c:n0 on 0 60 100 @1
c:a
c:vq
c:n off
```

---

## 3. Effect Card commands

Effect has no note bank. It covers phantom power, LEDs, audio enable,
I2C ADCs, USB ADC channel select, and RS485 echo.

| Command            | Meaning                                   |
| ------------------ | ----------------------------------------- |
| `h` / `help` / `?` | One-line menu                             |
| `s`                | Status: 48V, PG, audio, LED flash, echo   |
| `v`                | Read 48V enable and power-good            |
| `v 0` / `v 1`      | 48V off / on                              |
| `l 0` / `l 1`      | Auto LED flash off / on                   |
| `lr 0` / `lr 1`    | Red LED; `lr 1` also stops auto-flash     |
| `ly 0` / `ly 1`    | Yellow LED; same                          |
| `a 0` / `a 1`      | AUDIO_EN off / on                         |
| `i2c`              | Scan I2C2 (prints found addresses)        |
| `ai`               | Initialise both ADC chips                 |
| `ar <n> <reg>`     | Read ADC **1** or **2**, register 0..0xFF |
| `aw <n> <reg> <v>` | Write ADC register (value 0..0xFF)        |
| `u`                | Query USB ADC channel (1..8)              |
| `u <1..8>`         | Select USB ADC channel                    |
| `ec`               | Query RS485 keystroke echo                |
| `ec 0` / `ec 1`    | Echo off / on                             |

Default echo is **off**. Use `ec 1` only when you want the card to
echo keystrokes back on the bus.

ADC 7-bit addresses used at init: ADC1 `0x4C`, ADC2 `0x4D`.

On Effect, `ar` / `aw` read/write ADC registers. On Channel, `ar` sets
sample root pitch; sample selection is part of note-on and there is no `aw`
command. Always address a specific card on a shared bus.

### Effect quick examples

```text
e:s
e:v 1
e:a 1
e:ec 0
e:ai
e:ar 1 0
```

---

## 4. USB protocol (Channel Card)

The Channel Card exposes **one binary CDC ACM port**, VID/PID `cafe:4032`, with
serial `CHCARD-<96-bit UID>`. It has no USB Audio interface. The firmware owns a
small USB device/CDC implementation over STM32 HAL PCD; TinyUSB is removed from
this card. Effect Card retains its existing TinyUSB CDC/UAC microphone.

USB carries sample BODY, attacks, wavetables and scripts. RS485 retains note
commands, controls, diagnostics and `vq`. The Channel USB port accepts no ASCII
console commands. Open it in raw binary mode with software/hardware flow control
disabled, lower DTR, flush pending host data, then raise DTR and send HELLO.
CDC line coding is accepted but does not throttle USB to a UART baud rate.

### Framing (version 2)

All multi-byte values are little-endian. Every block is a five-byte header
followed by exactly `length` payload bytes (maximum 1024). Serial reads/writes
and the 64-byte Full-Speed USB transactions do not delimit application blocks.

| Offset | Bytes | Field |
|---|---:|---|
| 0 | 1 | Type: HELLO=1, BODY=2, UPLOAD_BEGIN=3, UPLOAD_DATA=4, UPLOAD_ABORT=5, REPLY=6, PROBE=7 |
| 1 | 1 | Target: voice, sample or logical wavetable according to type |
| 2 | 1 | BODY session 0..254; zero for other host requests; original request type for REPLY |
| 3 | 2 | Payload length (0..1024) |

There is no padding, idle packet or application CRC. USB bulk provides link-level
error detection/retry. When no voices require samples and no upload is pending,
the host submits no data blocks. Note release tails continue receiving BODY until
`vq` reports the voice inactive; `n off` retains its existing hard-stop behavior.

Version 2 requires a matching host and firmware; version 1's eight-byte framing
is incompatible. The first request is HELLO with target/session zero and the
single payload byte `02`. Its 14-byte reply payload is:

| Payload offset | Bytes | Value |
|---|---:|---|
| 0 | 1 | Status: 0 success |
| 1 | 1 | Protocol version: 2 |
| 2 | 1 | Voices: 8 |
| 3 | 1 | Source sample width: 1 signed-int8 byte |
| 4 | 4 | DAC rate: 48000 Hz |
| 8 | 2 | Maximum payload: 1024 bytes |
| 10 | 2 | Note priming: 998 samples |
| 12 | 2 | Current processed BODY counter, modulo 65536 |

REPLY echoes the target and puts the original request type in header byte 2.
Payload byte zero is status (`0` success, `1` error); errors may append a
diagnostic string without a NUL terminator. Successful upload replies contain
only the status byte. No request ID is transmitted: exactly one request/reply
exchange may be outstanding, even while BODY is streaming. The host verifies
reply type, target, status and successful payload length. A timeout, disconnect
or unexpected reply faults the host connection; it must reopen before sending
another request. It never retries an ambiguously completed request on that link.
BODY does not get a per-block success reply: RS485 `vq` acknowledges progress.
A BODY error generates an error REPLY identifying BODY and faults the link.

A malformed header, invalid BODY fields or partial frame stalled for over one
second faults reception. The host must close/reopen, toggle DTR, negotiate HELLO
and obtain current `vq` status before arming new notes. No scanning inside binary
samples attempts to guess framing. USB reset and DTR transitions discard receive,
transmit and parser state and abort uploads. Uncommitted BODY is never published.
When a valid BODY block arrives before ring space is available, firmware retains
that entire block and stops consuming further USB bytes until it fits. This
backpressure cannot overwrite the ring. A rejection despite that capacity check
increments `full`/`drop` and faults the transport; it is not normal flow control.

### USB-only diagnostic

PROBE (`7`) requires HELLO first and target/session zero. A nonempty
payload (1..1024 arbitrary test bytes) is counted and discarded, with no reply.
An empty PROBE is an ordered barrier: its REPLY contains status `0`, total test
bytes (u32), test blocks (u32), and rolling FNV-1a hash (u32). Counters wrap at
2^32 and reset on HELLO; hash starts at 2166136261 and updates for each test
payload byte as `(hash XOR byte) * 16777619` modulo 2^32. Headers are excluded.
PROBE does not alter audio, upload, the BODY counter, or ring credit state.

PROBE remains supported by the firmware for protocol diagnostics. The production
host does not expose a benchmark API or command; playback uses HELLO, BODY and
upload messages. PROBE timings do not measure key-to-DAC latency.

### BODY and flow control

BODY payload contains 1..1024 signed-int8 samples for one voice. RS485 note-on
arms a pending session; its first matching BODY establishes note start while
that pending ring is empty. Later blocks append, including across promotion
to the playing note. There is no START flag or per-note sequence on USB.
The existing 998-sample startup gate is independent of transport block size.
Samples and the DAC are still 48 kHz source format/output; scripts control the
playback increment.

The ordered stream uses an implicit cumulative BODY counter, shared by all
voices and wrapping at 65536. Firmware increments it exactly once per processed
BODY, including valid stale-note blocks that it discards. Partial frames and
blocks retained waiting for ring space do not advance it. Uploads do not advance
it. The host assigns corresponding counts internally when preparing blocks;
they are not transmitted in the header.

After DTR resets the transport, HELLO reports the firmware's current count.
The host drops its previous in-flight history, sets its next count to that
baseline plus one, and then reads fresh `vq` credit before enabling streaming.
A pre-reconnect status snapshot is not a safe baseline: previously queued
complete blocks could have been processed since that snapshot. DTR, note
resets and diagnostic-counter clearing do not reset the BODY counter; MCU
initialization does. Reopening always negotiates the current baseline.

The existing RS485 status frame remains byte-for-byte compatible:

```text
0..1    a5 5a
2       43 ('C')
3       0c (status type)
4       active voice mask
5       pending voice mask
6..7    ring capacity u16 (4080)
8       wrapping status sequence u8
9       age of last processed BODY, rounded-up audio ms; 255 unknown/expired
10..11  cumulative processed BODY counter u16
12..59  eight six-byte records:
          session u8
          free space u13, five-ms source demand u12, remaining time u15
          packed little-endian; time units are 100 microseconds
60      0a
```

Free space and acknowledgement are captured in the same snapshot. Each fresh
snapshot grants `max(0, reported free - all unacknowledged samples for that
physical voice)`, including outstanding blocks from older sessions. Credit is
charged when a block is prepared, including partially written blocks. The host
then advances the playing voice's refill balance using the reported five-ms
demand, keeping the previous two-ms safety reserve. CDC prediction is anchored
at status receipt and capped at the next five milliseconds; it never uses a
synthetic isochronous packet clock. Fresh status reconciles the balance and all
unacknowledged samples. Firmware backpressure remains authoritative if a pitch
or script change makes consumption slower than predicted.

At most 8232 wire bytes of BODY are outstanding, tracked in up to 64 block
records. Small blocks do not prematurely exhaust an eight-packet window. Ready
blocks are batched into a USB write without waiting to fill a batch. Missing
status stops further prediction; nothing is sent merely to keep USB busy.

The host polls every 5 ms normally. For high per-voice demand it uses
`clamp(capacity * 5 / (4 * max_active_five_ms_demand), 1, 5)` milliseconds to
obtain roughly four snapshots per ring. It uses card-reported demand, not host
pitch calculations. Note commands retain serialized RS485 access and MIDI
commands take the next turn after an in-progress poll.

After a note-on ACK the USB worker wakes immediately using existing confirmed
credit. It prioritizes playing voices within 3 ms of their reported deadline,
then initial 998-sample priming, then other refills. Within a priority group,
voices with fewer current-session samples in flight come first, followed by
reported deadline and rotating ties. Upload blocks use spare capacity; playing
voices within 10 ms and initial priming take precedence over uploads. A running
forecast may wake the worker each millisecond, but no idle packets are sent.
Normal refills gather at least a five-ms demand block where capacity allows;
endangered voices and initial priming do not wait for that batching threshold.

The USB receive ISR rearms immediately into an 8192-byte queue in fast DTCM.
PCD DMA is disabled, so USB buffers do not need the uncached audio DMA region. A full queue
leaves OUT unarmed (NAK/backpressure), without losing bytes. Main-loop processing
is bounded to 4096 bytes per pass, with a maximum 1024-byte payload. The 4080
sample voice rings and existing hold/silence fallback on underrun are preserved.
USB throughput and latency require hardware measurement: a 12 Mbit/s line rate
is not 12 Mbit/s of sample payload, and bulk does not reserve a service interval.

### Uploads

One upload can be active, interleaved with BODY blocks. Every upload request has
one REPLY, including each data chunk. `UPLOAD_BEGIN` payload is `kind:u8,
total_bytes:u32`; target is the sample, logical wavetable or voice:

| Kind | Target | Size |
|---|---|---|
| 1 attack | sample 0..247 | 1..512 bytes |
| 2 wavetable | logical wave 0..7 | 2..512 bytes |
| 3 script | voice 0..7 | 20..16404 bytes, existing FWSC ABI2 container |

`UPLOAD_DATA` payload is `offset:u32` followed by 1..1020 bytes. Target must match
BEGIN, offset must equal the bytes already received, and data cannot exceed the
declared total. The host currently uses at most 512 data bytes per upload chunk.
Commit occurs on the final data chunk, and its success REPLY confirms commit.
`UPLOAD_ABORT` has no payload. Idle upload transactions time out after 5 seconds.
No raw upload bytes bypass the block parser. The old ASCII `al`, `wl` and
`vmload` wire transactions have been replaced by these blocks.

Attack uploads preserve existing live replacement behavior: bytes overwrite live
storage as received; abort does not roll them back, and committed length changes
only after completion. Wavetable uploads remain rejected while notes reference
the bank. Invalid/interrupted script replacements preserve the selected program.
Host PCM is replaced only after a successful attack upload.

### VM program validation

The Channel Card boots without an active note program. Until a valid script is
uploaded, note commands return `err:no-program`. Script uploads retain the
existing target/version, CRC, bytecode and runtime validation. Use RS485 `vm`,
`vm <voice>` and `vm mem` for status and faults.

One fixed-arena Berry VM roots eight independent program objects and gives each
voice 64 native float state slots. Upload is accepted only while no voice is
sounding; note-ons during transfer return `err:vm-busy`. Invalid or interrupted
replacement preserves the selected program. Protected native-argument faults
remain voice-local; allocation, GC, watchdog, or uncertain interpreter state
invalidates the shared VM and silences all voices. Programs are lost on reset.
`vm mem` reports arena current/peak/free and allocation/GC diagnostics.

ABI2 requires `on_note_on(key, velocity)`, `on_note_off()`, and
`on_ramp_end()`. Runtime scripts inspect raw keys and live amplitude through
`input()`, use allocation-free
`pow()` for tracking policy, configure wavetable layers with `osc()`, and
control the common envelope with `ramp(target, slope)`.

---

## 5. Half-duplex and key text

On RS485, send one command, wait for the tagged reply, then send the
next. Do not pile commands while an ACK is still due.

Prefer `e:ec 0` unless you are deliberately testing echo.

For notes, send the physical MIDI key as an integer from 0 through 127. The
loaded FWSC program owns mapping and tuning.
