# Freshwater card protocol

Host ↔ Channel Card and Effect Card over RS485 (and the same command
set over USB CDC). One ASCII line in, one reply line out. That is the
protocol — there is no separate binary control frame. CDC attack-head (`al`)
and VM program (`vmload`) sessions carry binary payloads after their ASCII
commands (§4).

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

On a single-card USB CDC link the prefix is optional; the card still
accepts `c:` / `e:` / `*:`.

### End of line

- **Host → card:** end the command with a single `\r`. Do not send `\r\n`
  for one command — both CR and LF are treated as end-of-line, so
  `\r\n` runs the line twice.
- **Card → host:** replies end with `\r\n`.

### Reply tags

| Path    | Reply shape                                                       |
| ------- | ----------------------------------------------------------------- |
| RS485   | `[C]` or `[E]` then the body (note the space after the bracket) |
| USB CDC | body only, no tag                                                 |

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
| `n0`…`n7 on <key> <velocity> @<session>` | Streamed note-on; bind BODY session 0…254 before ACK.          |
| `n0`…`n7 off`            | Turn that slot off.                                                            |
| `n off`                | Silence all 8.                                                                  |

Bare `n0`…`n7` is a syntax error.

Success reply for sets: `ok`.

Fractional Hz is intentional (e.g. `261.625565` for C4). Do not round
to integers if you care about equal temperament.

### Shared bank (248 sample attacks + 8 oscillator wavetables)

Eight voices (`n0`…`n7`) assign sample heads `0..247` (`aw <voice> <id>`).
The bank holds **256** signed-int8 heads of up to **512** samples (~10.7 ms @
48 kHz); IDs `248..255` are reserved for logical oscillator wavetables
`0..7`. Upload is USB CDC only (§4). Contents survive while powered and are
lost on reset.

| Command             | Meaning                                                             |
| ------------------- | ------------------------------------------------------------------- |
| `al <id> <nbytes>`  | **USB CDC only.** Load sample head `<id>` 0…247 (1…512 signed-int8 bytes) |
| `wl <wave> <nbytes>` | **USB CDC only.** Load logical oscillator wave 0…7 (2…512 signed-int8 bytes) |
| `ar <id> <Hz>`      | Set head `<id>`'s root pitch (Hz > 0)                               |
| `aw <v> <id>`       | Assign sample head `<id>` 0…247 to voice `<v>` 0…7                  |
| `a`                 | Loaded count + 256-bit hex mask (bit 0 = wave 0)                    |
| `vq`                | ABI1 active/pending generations + exact runtime ring credit             |
| `usb`               | BODY counters: drop/hold/fill, RS-485 `vq`, rx/bytes/bad              |
| `usb 0`             | Clear those counters, then same reply                                 |

Replies: `ok: ar <id> <Hz>`, `ok: aw <v> <id>`, `ok: a <n> <64 hex>`.
USB CDC returns a readable `ok:vq7` diagnostic. RS485 returns the fixed 56-byte
frame described below.

Playback pitch is on-card: `phase_inc = note_Hz / root_Hz`, 2-tap
linear interpolation. The attack plays to its committed length (not a hold-pad to
512). Body starts at `len − 32` with the same source index and
fraction as the attack. The host does not count body-FIFO consume
until that join. Every `nX on <key> [velocity]` command is a note-on, including key 0.

### Channel VM programs

| Command                    | Meaning                                                          |
| -------------------------- | ---------------------------------------------------------------- |
| `vmload <voice> <nbytes>`  | **USB CDC only.** Upload one FWSC ABI1 program to voice 0…7      |
| `vm`                       | Query the active-program voice mask                              |
| `vm <voice>`               | Query active state, ABI target/version, and fault for one voice  |
| `vm mem`                   | Shared-arena metrics followed by eight per-voice diagnostic lines |
| `cpuload [0\|1]`           | Query or enable LED_Y/PB9 DMA-refill duty-cycle probe              |

`vmload` accepts a complete 20…16404-byte FWSC container and then switches CDC
from line parsing to binary input. The full transaction is specified in §4.

### VM-controlled amplitude envelope

Amplitude ramps and note lifecycle are controlled only by the uploaded per-voice
VM program. The firmware exposes no separate envelope-programming commands.
See [SCRIPTING.md](../../cmi_core/SCRIPTING.md) and `vmload` below.

Each `osc(wave, frequency_hz)` call appends a pending-note oscillator and
returns an opaque handle that may be assigned or ignored. Logical waves `0..7`
map to attack-bank IDs `248..255`; repeated calls may reuse a wave. Handles
remain valid through note promotion for future targeted modulation. The sample
and enabled oscillators are averaged before the existing filter and envelope.

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
c:aw 0 0
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

Note the card-local meaning of `ar` / `aw`: on Effect they are ADC
register read/write; on Channel they are sample root-pitch / wave
assignment. Always address a specific card on a shared bus.

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

Channel Card USB exposes **two** host-facing functions. Do not conflate them:

| Interface                    | Role                                                                |
| ---------------------------- | ------------------------------------------------------------------- |
| **UAC2 output** (ITF0/1)     | 21-channel signed 8-bit, synchronous 48 kHz carrier; every 1 ms packet carries four metadata bytes and 1004 raw 48 kHz BODY samples (1008 B). |
| **CDC ACM** (serial)         | Same ASCII console as RS485, plus binary attack-head upload (`al`). |

The Channel device is class-compliant UAC2 so the operating system owns the
audio endpoint lifecycle; there is no custom vendor/libusb isochronous pipe.
The audio samples are transport words, not audible multichannel PCM. A primed
BODY underrun increments `hold`, resets/mutes the DAC, latches the fault LEDs,
and halts the Channel Card. A ring-capacity rejection does the same after
incrementing `drop`. Reset or power-cycle the card after either fault.
The UAC topology has no Feature Unit and exposes no mute or volume controls.
Each complete 21-channel int8 frame is independently routable. The host audio
callback may produce multiple milliseconds at once, but USB transmits fixed
48-frame, 1008-byte windows every millisecond.

CDC is a separate function. `al` still uses CDC. Keep that port closed during
BODY streaming unless console or attack-head traffic is required.

Effect Card still has CDC and a UAC2 microphone (mono, 32-bit, 96 kHz).

### BODY stream (UAC2 iso OUT, signed 8-bit)

The first byte is a routing tag, the second is the session, the next two are a
little-endian wrapping transport sequence, and the remaining 1004 bytes are
raw signed-int8 BODY samples. `vq`
reports the last routed sequence processed by the card, whether accepted or
rejected, alongside an atomic ring-credit snapshot. The host ledger subtracts
exactly the later submitted frames; it never estimates in-flight occupancy.

```text
UAC carrier: 48 kHz × 21 channels × int8 = 1008000 bytes/s
BODY/DAC:    48 kHz (pitch and playback rate are unchanged)
packet byte 0       tag = 0xA0 | SOF[3] | voice[2:0]
packet byte 1       session
packet bytes 2..3   wrapping uint16 LE transport sequence
packet bytes 4..1007  1004 consecutive signed-int8 unpitched BODY samples
idle tag      0xFF (remaining bytes are zero)
session       0..254 (255 is reserved for idle/unarmed)
```

VID `0xCafe`, PID `0x4031`. The host opens the 21-channel UAC output through
RtAudio/CoreAudio; CDC stays a serial port.

The direct carrier supplies 1004 BODY samples/ms (1004 ksample/s aggregate).
Workloads above that source-consumption ceiling cannot be lossless; `hold`
records missing playback samples while control remains alive. Each fresh `vq`
uses exact safe ring credit up to that wire ceiling.
RS485 `vq` every 5 ms is the steady-state refill authority and lifecycle
monitor. UAC OUT carries BODY data only.

```text
RS485 vq reply, fixed 56-byte ABI1 binary frame
offset  size  field
0       2     sync = a5 5a
2       1     card = 43 ('C')
3       1     type = 04 sequenced ABI1 generation status
4       1     active voice mask
5       1     pending voice mask
6       1     best refill voice, or 255
7       1     reserved
8       2     runtime ring capacity (uint16 LE; normally 4080)
10      2     status sequence (uint16 LE)
12      2     last processed UAC sequence
14      40    eight records: session u8, target fill u16 LE, total writable u16 LE
54      1     CRC-8/0x07 over bytes 0..53
55      1     terminator = 0a
```

RS485 `vq` is the sole refill permission. The host sends only complete
1004-sample UAC frames within its reported credit. A streamed `nX` first arms a
pending generation; matching SOF data fills it, and Berry receives
`on_note_on` only after a complete BODY frame exists. Berry then decides when
to call `start_note()`, which atomically promotes pending. No native crash
duration or release reservation exists.
`start_note(frequency)` may atomically replace the pending playback pitch while
promoting it; zero-argument `start_note()` keeps the default. `pitch_for_key(key)`
returns standard MIDI pitch (A4 = 440 Hz) when a script wants a reference;
direct Hz selection may ignore it.
Superseded or late same-wave sessions are stale. Untagged `nX` is available
for direct console use. There is no native release reservation.
RS485 `nX off` remains note-off authority. `type` `0x20` CAPTURE remains reserved.
An explicit note-off cancels any pending replacement in firmware before the
script's zero-argument `on_note_off()` handler runs.
Every routed UAC frame has a wrapping sequence. The card reports the last
processed sequence with the same ring snapshot, and the host subtracts exactly
the later frames in its ledger. Playing voices are scheduled by depletion
deadline. A silent voice is admitted only when its exact one-packet (1 ms)
service cost finishes before the earliest playing deadline.
The controller targets one `vq` every 5 ms (200 Hz) and requests an immediate
one after successful Channel commands; single-flight RS485 traffic can stretch
the observed interval. The direct UAC capacity is 1004 BODY samples/ms: four
metadata bytes and 1004 sample bytes in each 1008-byte packet.
If note-off or a newer generation retires a ring while a prior-session frame is in
flight, the card acknowledges the routed sequence but rejects its stale
session instead of publishing it into the new ring.

The `usb` bad-reason fields are reserved and read zero for the direct
transport. Ring-capacity rejection remains visible as `drop` and
playback starvation as `hold`.

### CDC vs RS485 (console)

|                | RS485                                       | USB CDC (ACM)                                                                  |
| -------------- | ------------------------------------------- | ------------------------------------------------------------------------------ |
| Commands       | Same parsers                                | Same parsers                                                                   |
| Line framing   | Host `\r`; card `\r\n`                      | Same                                                                           |
| Address prefix | Useful on a shared bus (`c:` / `e:` / `*:`) | Optional; still accepted                                                       |
| Reply tag      | `[C]` / `[E]`                             | None — body only                                                               |
| Echo           | Channel: off. Effect: `ec` (default off)    | Local keystroke echo on                                                        |
| Baud           | **921600 8N1** on the UART                  | Host may open any rate (e.g. 115200); TinyUSB CDC ignores line coding for data |
| `al`           | Rejected (`err:usb`)                        | Allowed                                                                        |
| `vmload`       | Rejected (`err:usb`)                        | Allowed                                                                        |

Typical host path on macOS / Linux: Channel `cu.usbmodem*` / `ttyACM*`.
USB–UART adapters (`cu.usbserial*`, `ttyUSB*`) are the RS485 dongle — wrong
port for uploads.

### Upload sessions (`al` and `wl`)

Binary payload rides on the **same** CDC pipe as ASCII. After `ok:ready`,
the console leaves line mode: CDC RX bytes go to the upload sink only
(no echo, no `\r` line framing, no command parse) until the byte count
is met.

Constraints:

| Command                 | Payload                                               |
| ----------------------- | ----------------------------------------------------- |
| `al <id> <nbytes>`      | Sample ID 0…247; 1…512 signed-int8 bytes              |
| `wl <wave> <nbytes>`    | Logical oscillator wave 0…7; 2…512 signed-int8 bytes  |

The host never supplies a physical bank ID for an oscillator. Channel firmware
maps `wl 0..7` into its reserved storage and returns the same logical number.
`al` cannot write reserved wavetable storage. Uploads are rejected while
active, pending, or queued notes may reference the bank. Oscillator tables
must contain 2…512 periodic samples.

**Console reply API (what the card writes):** exactly two success lines
for a full transfer — nothing in between, even if USB delivers the
payload in many reads.

| When                                  | Card writes                                     |
| ------------------------------------- | ----------------------------------------------- |
| `al` or `wl` accepted                 | `ok:ready\r\n`                                  |
| During the `nbytes` of payload        | *(no console reply)*                            |
| Last `al` byte committed              | `ok:attack <id>\r\n`                            |
| Last `wl` byte committed              | `ok:wavetable <wave>\r\n`                      |

There is no per-chunk ACK. Mid-payload console traffic would serialize
Full-Speed CDC and is intentionally omitted.

Sequence:

```text
Host →  al 0 512\r
Card →  ok:ready\r\n          ← console write #1
Host →  <512 raw bytes>       ← opaque to the console parser
Card →  ok:attack 0\r\n       ← console write #2

Host →  wl 0 512\r
Card →  ok:ready\r\n
Host →  <512 periodic raw bytes>
Card →  ok:wavetable 0\r\n
```

Errors you may see instead of the ready/completion replies:

| Body         | Meaning                                          |
| ------------ | ------------------------------------------------ |
| `err:usb`    | `al` or `wl` sent on RS485                       |
| `err:syntax` | Bad command line                                 |
| `err:range`  | Id / size / concurrent upload / commit failure   |

Notes:

1. Keep the CDC session open across ids when loading a bank; reopen cost
   dominates, not the transfer.
2. While waiting for the completion line, keep reading the CDC RX path —
   do not discard pending replies.
3. Upload only loads AXI RAM (lost on reset). Set `ar <id> <rootHz>`; the host
   app starts it with RS485 `aw <voice> <id>` then session-bound
   `nX on <key> <velocity> @<session>` immediately after launching its USB BODY job;
   later sustain uses vq-authorized BODY refills.

### VM program upload (`vmload`)

The Channel Card boots with no active note program. Until a valid program is
uploaded, note commands return `err:no-program` and the note bank stays silent.

`vmload <voice> <nbytes>` is CDC-only and uses the same line-to-binary
transition as `al`. Each voice 0..7 owns an independent program. The payload is
one Berry ABI1 `FWSC` container, up to 16404 bytes (20-byte container header
plus at most 16384 payload bytes). The card writes
`ok:ready`, receives exactly `nbytes`, validates target/version, CRC, bytecode,
runtime/configuration, handlers, host calls, and CRC, then replies
`ok:vm <voice> <target> <version>`. `vm` reports the active voice mask;
`vm <voice>` reports `ok:vm <voice> inactive <fault>` or
`ok:vm <voice> active <target> <version> <fault>`.

One fixed-arena Berry VM roots eight independent program objects and gives each
voice 64 native float state slots. Upload is accepted only while no voice is
sounding; note-ons during transfer return `err:vm-busy`. Invalid or interrupted
replacement preserves the selected program. Protected native-argument faults
remain voice-local; allocation, GC, watchdog, or uncertain interpreter state
invalidates the shared VM and silences all voices. Programs are lost on reset.
`vm mem` reports arena current/peak/free and allocation/GC diagnostics.

ABI1 requires `on_note_on(key, velocity)`, `on_note_off()`, and
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
