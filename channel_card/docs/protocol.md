# Freshwater protocol

Host ↔ Channel Card and Effect Card over RS485 (and the same command
set over USB CDC). One ASCII line in, one reply line out. That is the
protocol — there is no separate binary control frame; the only binary
payloads are the CDC attack-head uploads (`al`, §4).

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
c:n0 440
e:s
*:h
n0 440
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
| `err:usb`                               | CDC-only command (`al`) sent on RS485         |
| `err:rxdrop`                            | Channel UART RX overrun between lines         |
| `err: ar …` / `err: aw …` / `err: adc…` | Effect I2C / ADC failure                      |

---

## 2. Channel Card commands

Eight note slots: `n0` … `n7` (voices 0–7). Slot digits `8`–`f` parse
but reply `err:range`. All voices mix onto DAC channel 1.

Each voice is a **SAMPLE voice**: note-on plays the assigned attack head
from AXI RAM, then the USB BODY slots, through one on-card playhead
(pitch, filter, envelope). Host streams unpitched body; the card rate-scales
(`note_Hz / root_Hz`). Voices without a loaded wave synthesize the global
DDS shape (`s` / `p` / `t`). Envelope (`en`) and per-voice LPF (`f`) apply
to either source.

Boot / bare `n0` also turns the analog bypass path on and sets CH1 DAC
trim to 0 dB (`g 1 0`). Frequency changes do not touch gain or bypass.

### Help

| Command            | Reply                 |
| ------------------ | --------------------- |
| `h` / `help` / `?` | One-line command list |

### Notes

| Command                | Meaning                                                                           |
| ---------------------- | --------------------------------------------------------------------------------- |
| `n0`                   | Session defaults only (bypass on, `g 1 0`). Does not start a tone.                |
| `n0`…`n7 <Hz> [scale]` | Set that slot. Hz in **[20, 20000)**. Scale in **[0, 1]**; if omitted, **0.125**. |
| `n0`…`n7 <Hz> <scale> @<session>` | Streamed note-on; bind BODY session 0…254 before ACK. |
| `n0`…`n7 0`            | Turn that slot off (Hz ≤ 0).                                                      |
| `n <Hz> [scale]`       | Same rules for all 8 slots.                                                       |
| `n 0`                  | Silence all 8.                                                                    |

Bare `n1`…`n7` (no Hz) is a syntax error. Only bare `n0` is the session
shortcut.

Success reply for sets: `ok`.

Fractional Hz is intentional (e.g. `261.625565` for C4). Do not round
to integers if you care about equal temperament.

### Sample bank (256 AXI attack heads)

Eight voices (`n0`…`n7`) assign any stored head (`aw <voice> <id>`).
The bank holds **256** int16 heads of up to **512** samples (~10.7 ms @
48 kHz). Upload is USB CDC only (§4). Contents survive while powered
and are lost on reset.

| Command             | Meaning                                                             |
| ------------------- | ------------------------------------------------------------------- |
| `al <id> <nbytes>`  | **USB CDC only.** Load head `<id>` 0…255 (2…1024 bytes int16 LE)    |
| `ar <id> <Hz>`      | Set head `<id>`'s root pitch (Hz > 0)                               |
| `aw <v> <id>`       | Assign head `<id>` to voice `<v>` 0…7                               |
| `a`                 | Loaded count + 256-bit hex mask (bit 0 = wave 0)                    |
| `vq`                | Active mask + hungriest + exact BODY-ring credit                      |
| `crash [0..50]`     | Release before stolen-slot retrigger, ms; 0 = hard cut (default 3)      |
| `usb`               | BODY counters: drop/hold/fill, RS-485 `vq`, rx/bytes/bad              |
| `usb 0`             | Clear those counters, then same reply                                 |

Replies: `ok: ar <id> <Hz>`, `ok: aw <v> <id>`, `ok: a <n> <64 hex>`.
USB CDC returns `vq` as `ok:vq <mask> <best> free0 … free7 <reserved>`.
RS485 returns the same fields in a 26-byte binary frame: `a5 5a 43 02`,
mask, best, eight uint16 LE exact free-sample counts, uint16 LE reserved value
`0xffff`, CRC-8/0x07, then `0a`. Best is 0–7 or 255; each free
count is 0–12240. At 921600 8N1 the reply is about 282 µs on the wire.

Playback pitch is on-card: `phase_inc = note_Hz / root_Hz`, 2-tap
linear interpolation. The attack plays to its committed length (not a hold-pad to
512). Body starts at `len − 32` with the same source index and
fraction as the attack. The host does not count body-FIFO consume
until that join. `nX > 0` is always a note-on.

### Oscillator shape (global)

| Command        | Meaning                                           |
| -------------- | ------------------------------------------------- |
| `s`            | Sine                                              |
| `p <0.1..0.9>` | Pulse; argument is duty                           |
| `t <0.1..0.9>` | Triangle; argument is asymmetry (0.5 = symmetric) |

Applies to voices whose assigned wave id has no loaded attack head.

### Amplitude envelope

Each voice can have a multi-segment linear envelope: pairs of
`(end_amp, slope[±k])` then a final `release_slope[±k]`. Start of the first
segment is always 0. Last segment is release to 0.

| Command               | Meaning                                |
| --------------------- | -------------------------------------- |
| `en`                  | List which slots have a program        |
| `en <tokens…>`        | Program all 8 voices                   |
| `en 0`                | Clear all voices (unprogrammed bypass) |
| `en0`…`en7`           | Query one voice                        |
| `en0`…`en7 <tokens…>` | Program one voice                      |
| `en0`…`en7 0`         | Clear one voice (unprogrammed bypass)  |

Slot digits `8`–`f` are accepted by the parser (the envelope bank is
16 deep for historical reasons) but drive no audible voice.

Token list rules:

- Clear: single token `0`.
- Program: odd count, at least 3 tokens, at most 19.
- Pattern: `end slope[±k] [end slope[±k] …] release_slope[±k]`
- Each `end` in **[0, 1]**; each `slope` **> 0** (amplitude units per second)
- Optional pitch-track constant glued to the slope token: `10+1`, `2.0-0.5`
  (no spaces). Omit the suffix for `k = 0`.
- Segment count ends up between 2 and 10 (including release)

Examples:

```text
en0 1.0 10  0.2
en0 1.0 10+1  0.7 5  0.2
en0 1.0 2.0+2  0.2-1
en0 0
en 0
```

Unprogrammed voices leave amplitude at full scale (envelope bypass).

### Envelope pitch tracking

Per-segment `k` is set on the `en` slope token (above). Each segment’s rate
is scaled by `(note_Hz / C4)^k` with C4 = 261.625565 Hz. Same idea as filter
`fk` below, but it changes timing, not cutoff. Negative `k` lengthens
higher notes.

| Command                       | Meaning                                      |
| ----------------------------- | -------------------------------------------- |
| `ek`                          | Dump k for all 8 voices                      |
| `ek <k>`                      | Set the same k on all segments of all voices |
| `ek0`…`ek7` / `ek0`…`ek7 <k>` | Query / bulk-set one voice                   |

`k` is in **[−10, 10]**. Default **0** (no pitch effect). `ek` is a bulk
override; prefer `slope±k` on `en` for per-segment values. Query prints one
value when all segments share k, otherwise one k per segment.

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

### CPU load probe (bring-up)

Drives N note-bank voices and lights LED_Y while the main loop fills
audio. Not a normal musical control.

| Command             | Meaning                              |
| ------------------- | ------------------------------------ |
| `cpu`               | On, all 8 voices, DMA-style probe    |
| `cpu N`             | On, N voices (1..8)                  |
| `cpu q` / `cpu q N` | Soft-queue style probe               |
| `cpu 0`             | Off; clear notes; LED chaser resumes |

### Removed commands

`m` / `mode`, `w0`…`w7`, `wl`, `sw`, `tone1`, `dc`, `scf`, `duty` and
`gain` no longer exist; they reply `err:unknown` (or parse as another
command). Attack upload uses `al`; the former `bl` body upload is retired
and replies `err:unsupported`.

### Channel quick examples

```text
c:n0
c:g 1 0
c:s
c:n0 440
c:p 0.5
c:f0 300
c:fk0 1
c:n0 523.25
c:en0 1.0 10+1 0.2
c:aw 0 0
c:a
c:vq
c:n 0
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
| **UAC2 output** (ITF0/1)     | 10-channel signed 16-bit, synchronous 51 kHz carrier; every frame carries a tag and nine raw 48 kHz BODY samples (1020 B / 1 ms). |
| **CDC ACM** (serial)         | Same ASCII console as RS485, plus binary attack-head upload (`al`). |

The Channel device is class-compliant UAC2 so the operating system owns the
audio endpoint lifecycle; there is no custom vendor/libusb isochronous pipe.
The audio samples are transport words, not audible multichannel PCM. There is
no inner packet, length, or CRC: USB audio-frame boundaries provide the
framing. A BODY underrun increments `hold` without disabling USB, RS485, or
audio; a later authorized frame can recover.
The UAC topology has no Feature Unit and exposes no mute or volume controls.
Each complete 10-channel int16 frame is independently routable. The host audio
callback may produce multiple milliseconds at once, but USB transmits fixed
51-frame, 1020-byte windows every millisecond.

CDC is a separate function. `al` still uses CDC. Keep that port closed during
BODY streaming unless console or attack-head traffic is required.

Effect Card still has CDC and a UAC2 microphone (mono, 32-bit, 96 kHz).

### BODY stream (UAC2 iso OUT, signed 16-bit)

Little-endian. BODY has no per-frame ACK. Channel 0 is a compact routing tag;
channels 1..9 are nine consecutive raw BODY samples. Sequential RS485 `vq`
polling supplies exact ring free space every 5 ms for steady-state refills.
Urgent SOF frames launch concurrently with the corresponding RS485 `aw`/`nX`
transaction and do not wait for `vq`; their conservative credit accounts for
the configured old-note crash-release tail.

```text
UAC carrier: 51 kHz × 10 channels × int16 = 1020000 bytes/s
BODY/DAC:    48 kHz (pitch and playback rate are unchanged)
frame word 0  tag = 0xA000 | session[11:4] | SOF[3] | voice[2:0]
frame words 1..9  nine consecutive int16 LE unpitched BODY samples
idle tag      0xAFFF (remaining words are zero)
session       0..254 (255 is reserved for idle/unarmed)
```

VID `0xCafe`, PID `0x402F`. The host opens the 10-channel UAC output through
RtAudio/CoreAudio; CDC stays a serial port.

The direct carrier supplies 459 BODY samples/ms (459 ksample/s aggregate).
Workloads above that source-consumption ceiling cannot be lossless;
`hold` records missing playback samples while control remains alive. Each fresh
`vq` uses exact safe ring credit up to that wire ceiling.
RS485 `vq` every 5 ms is the steady-state refill authority and lifecycle
monitor. UAC OUT carries BODY data only.

```text
RS485 vq reply, fixed 26-byte binary frame
offset  size  field
0       2     sync = a5 5a
2       1     card = 43 ('C')
3       1     type = 02 exact refill status
4       1     active/requested voice mask
5       1     hungriest voice, or 255
6       16    exact uint16 LE free samples for voices 0..7 (0..12240)
22      2     reserved = ffff
24      1     CRC-8/0x07 over bytes 0..23
25      1     terminator = 0a
```

RS485 `vq` supplies an exact free-space snapshot for steady refills. The host
subtracts direct UAC samples rendered concurrently with that snapshot so it
cannot over-reserve the physical ring. RS485 `nX <Hz>` remains the sole audible
note-on and pitch authority. For streamed playback, the host reserves a
session and launches `aw` plus `nX <Hz> <scale> @<session>` concurrently with
about 25 ms of pitch-adjusted SOF BODY, filling the ring during the attack.
Superseded or late same-wave sessions are stale. Untagged `nX` remains available
for interactive/legacy use. If a
conservative 50 ms release reservation leaves no safe prefill credit, USB
sends no tagged frame; the next `vq` supplies the first SOF BODY normally.
RS485 `nX 0` remains note-off authority. `type` `0x20` CAPTURE remains reserved.
Urgent jobs form a newest-first priority deque with one latest job per physical
voice (maximum depth eight). A newer note retires older tagged frames; frames
already entering USB are rejected as stale by voice/session if superseded.
Every refill uses exact credit up to its per-voice and wire limits.
The controller targets one `vq` every 5 ms (200 Hz) and requests an immediate
one after successful Channel commands; single-flight RS485 traffic can stretch
the observed interval. The direct UAC capacity is 459 BODY samples/ms: 51
audio frames/ms times nine samples/frame.
If note-off or a newer note generation retires a ring while an old frame is
arriving, the card ignores that stale voice/session instead of publishing it
into the new generation.

The legacy `usb` bad-reason fields remain in the console ABI but read zero for
the direct transport. Ring-capacity rejection remains visible as `drop` and
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

Typical host path on macOS / Linux: Channel `cu.usbmodem*` / `ttyACM*`.
USB–UART adapters (`cu.usbserial*`, `ttyUSB*`) are the RS485 dongle — wrong
port for uploads.

### Upload session (`al`)

Binary payload rides on the **same** CDC pipe as ASCII. After `ok:ready`,
the console leaves line mode: CDC RX bytes go to the upload sink only
(no echo, no `\r` line framing, no command parse) until the byte count
is met.

Constraints:

| Command            | Payload                                               |
| ------------------ | ----------------------------------------------------- |
| `al <id> <nbytes>` | 2…1024 bytes = 1…512 × int16 LE (real length, no hold-pad) |

Wave `<id>` is **0…255**. Voice assign is `aw <voice> <id>`.

**Console reply API (what the card writes):** exactly two success lines
for a full transfer — nothing in between, even if USB delivers the
payload in many reads.

| When                                  | Card writes                                     |
| ------------------------------------- | ----------------------------------------------- |
| `al` accepted                         | `ok:ready\r\n`                                  |
| During the `nbytes` of payload        | *(no console reply)*                            |
| Last byte received and bank committed | `ok:attack <id>\r\n`                            |

There is no per-chunk ACK. Mid-payload console traffic would serialize
Full-Speed CDC and is intentionally omitted.

Sequence:

```text
Host →  al 0 1024\r
Card →  ok:ready\r\n          ← console write #1
Host →  <1024 raw bytes>      ← opaque to the console parser
Card →  ok:attack 0\r\n       ← console write #2
```

Errors you may see instead of `ok:ready` / `ok:attack`:

| Body         | Meaning                                          |
| ------------ | ------------------------------------------------ |
| `err:usb`    | `al` sent on RS485                               |
| `err:syntax` | Bad command line                                 |
| `err:range`  | Id / size / concurrent upload / commit failure   |

Notes:

1. Keep the CDC session open across ids when loading a bank; reopen cost
   dominates, not the transfer.
2. While waiting for the completion line, keep reading the CDC RX path —
   do not discard pending replies.
3. Upload only loads AXI RAM (lost on reset). Set `ar <id> <rootHz>`; the host
   app starts it with RS485 `aw <voice> <id>` then session-bound
   `nX <Hz> <scale> @<session>` immediately after launching its USB BODY job;
   later sustain uses vq-authorized BODY refills.

---

## 5. Half-duplex and pitch text

On RS485, send one command, wait for the tagged reply, then send the
next. Do not pile commands while an ACK is still due.

Prefer `e:ec 0` unless you are deliberately testing echo.

For pitch, send the real frequency as text (`261.625565`), not a rounded
integer, if you care about octaves lining up.
