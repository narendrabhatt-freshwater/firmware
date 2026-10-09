# Channel Card wire protocol

Channel Card controls use RS485 commands and replies. USB is a single binary
CDC stream for samples and uploads (§3); it has no USB text console. Other cards
may share the RS485 bus, so address Channel commands explicitly with `c:`.

The default RS485 baud rate is **921600 8N1**; `BAUDRATE` in `app/Makefile` selects
a different firmware build rate. The host must use the same rate.

Type `c:h` (or `c:help` or `c:?`) for the firmware menu. The menu retains
legacy `al`, `wl`, and `vmload` names: these return `err:usb` over RS485; use
binary upload kinds 1–3. There is no ASCII upload path on USB.

---

## 1. How a line works

1. You send one command line.
2. The card runs it.
3. The card sends a tagged reply. `vm mem` returns multiple lines; `vq` returns
   its fixed binary status frame.

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
c:h
c:vm
c:ar 0 261.625565
```

### End of line

- **Host → card:** end the command with a single `\r`. The parser also
  accepts `\n`; empty lines are ignored.
- **Card → host:** replies end with `\r\n`.

### Reply tags

| Path    | Reply shape                                                       |
| ------- | ----------------------------------------------------------------- |
| RS485 text | `[C] ` followed by the body |
| RS485 `vq` | Fixed 61-byte binary status, without a text tag |

Examples of bodies (tag omitted for clarity):

```text
ok
ok:vm mask 01
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
| `err:usb` | Legacy ASCII upload requested; use the binary upload protocol |
| `err:busy` | A voice event could not be queued |
| `err:rxdrop`                            | Channel UART RX overrun between lines         |
| `err:no-program`                        | Channel voice has no active VM program        |
| `err:vm-busy`                           | Note/program operation conflicts with VM upload or active playback |

---

## 2. Console commands

The [Channel command reference](README.md#console-command-reference) lists
syntax, ranges, and examples. Select sample heads 0..247 in the explicit
`n0 on <sample> <key> <velocity> @<session>` form. Logical wavetables 0..7
occupy bank entries 248..255; there is no `aw` command.

Text status includes `vm`, `vm <voice>`, and `vm mem`. The `vq` reply is binary
and is defined below. `clear` and `n off` hard-stop voices and reset Berry state
at the next audio boundary, retaining loaded samples/programs and USB link state.

## 3. USB protocol

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
An idle voice waits for 998 pending BODY samples before dispatching note-on.
For an already playing voice, note-on starts its script immediately;
`start_note()` promotes the pending queue when the script finishes its steal
fade. Current-session BODY remains accepted until that switch. Current and
pending queues share the 4080-byte pool, allocated in 16-byte blocks.
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

The dual-session RS485 status response is 61 bytes with type `0x0E`.
Its voice records differ from the older `0x0C` format; host and firmware must
use the same layout:

```text
0..1    a5 5a
2       43 ('C')
3       0e (dual-session status type)
4       active voice mask
5       pending voice mask
6..7    ring capacity u16 (4080)
8       wrapping status sequence u8
9       age of last processed BODY, rounded-up audio ms; 255 unknown/expired
10..11  cumulative processed BODY counter u16
12..59  eight six-byte records:
          target session u8, current session u8
          free space u12, five-ms source demand u9, remaining time u11
          packed little-endian; time units are 100 microseconds
          remaining time is rounded down and clamped at 204.7 ms
60      0a
```

Free space and acknowledgement are captured in the same snapshot. Each fresh
snapshot grants `max(0, reported free - all unacknowledged samples for that
physical voice)`, including outstanding blocks from older sessions. Reserve each BODY payload
rounded up to a multiple of 16 samples, matching the ring allocator. Credit is
charged when a block is prepared, including partially written blocks. Fresh
status reconciles credit with the cumulative BODY count. Firmware backpressure
remains authoritative if a pitch or script change slows consumption.

Host scheduling must keep outstanding sample reservations within the latest
confirmed per-voice credit. Poll `vq` frequently enough for the current playback
rate, account for unacknowledged BODY blocks, and continue refills through release
tails. Scheduling intervals and batching policies belong to the host implementation.

The USB receive path publishes received bytes into an 8192-byte queue in DTCM.
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
declared total. Commit occurs on the final data chunk, and its success REPLY confirms commit.
`UPLOAD_ABORT` has no payload. Idle upload transactions time out after 5 seconds.
No raw upload bytes bypass the block parser. The old ASCII `al`, `wl` and
`vmload` wire transactions have been replaced by these blocks.

Attack uploads preserve existing live replacement behavior: bytes overwrite live
storage as received; abort does not roll them back, and committed length changes
only after completion. Wavetable uploads remain rejected while notes reference
the bank. Invalid/interrupted script replacements preserve the selected program.

### VM program validation

The Channel Card boots without an active note program. Until a valid script is
uploaded, note commands return `err:no-program`. Script uploads retain the
existing target/version, CRC, bytecode and runtime validation. Use RS485 `vm`,
`vm <voice>` and `vm mem` for status and faults.

One fixed-arena Berry VM roots eight independent program objects and gives each
voice 64 native float state slots. Upload is accepted only while no voice is
sounding; note-ons during transfer return `err:vm-busy`. Invalid or interrupted
replacement preserves the selected program. Protected native-argument faults
remain voice-local; allocation, GC, or uncertain interpreter state
invalidates the shared VM and silences all voices. Programs are lost on reset.
`vm mem` reports arena current/peak/free and allocation/GC diagnostics.

ABI2 requires `on_note_on(key, velocity)`, `on_note_off()`, and
`on_ramp_end()`. Runtime scripts inspect raw keys and live amplitude through
`input()`, use allocation-free
`pow()` for tracking policy, configure wavetable layers with `osc()`, and
control the common envelope with `ramp(target, slope)`.

---

## 4. Half-duplex and key text

On RS485, send one command, wait for the tagged reply, then send the
next. Do not pile commands while an ACK is still due.

Keep other devices' keystroke echo disabled on the shared RS485 bus.

For notes, send the physical MIDI key as an integer from 0 through 127. The
loaded FWSC program owns mapping and tuning.
