# Freshwater protocol

Host ↔ Channel Card and Effect Card over RS485 (and the same command
set over USB CDC). One ASCII line in, one reply line out. That is the
protocol — there is no separate binary control frame.

Baud on the RS485 UARTs is **460800 8N1**.

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
| RS485   | `[C] ` or `[E] ` then the body (note the space after the bracket) |
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
| `err:rxdrop`                            | Channel UART RX overrun between lines         |
| `err: ar …` / `err: aw …` / `err: adc…` | Effect I2C / ADC failure                      |

There is no binary command frame. Notes are ASCII Hz text only.

---

## 2. Channel Card commands

Sixteen note slots: `n0` … `nf` (hex digits `0`–`f` = voices 0–15).
They mix onto DAC channel 1. Global shape (`s` / `p` / `t`) applies to
all slots. Digital LPF commands cover voices **0–7** only
(`f0`…`f7`, `fk0`…`fk7`); voices 8–15 stay at boot bypass.

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
| `n0`…`nf <Hz> [scale]` | Set that slot. Hz in **[20, 20000)**. Scale in **[0, 1]**; if omitted, **0.125**. |
| `n0`…`nf 0`            | Turn that slot off (Hz ≤ 0).                                                      |
| `n <Hz> [scale]`       | Same rules for all 16 slots.                                                      |
| `n 0`                  | Silence all 16.                                                                   |

Bare `n1`…`nf` (no Hz) is a syntax error. Only bare `n0` is the session
shortcut.

Success reply for sets: `ok`.

Fractional Hz is intentional (e.g. `261.625565` for C4). Do not round
to integers if you care about equal temperament.

### Playback mode

| Command | Meaning                                          |
| ------- | ------------------------------------------------ |
| `m`     | Query: `ok: m 0` (notes) or `ok: m 1` (wave)     |
| `m 0`   | DDS note bank (`s`/`p`/`t`, `n0`…`nf`) — default |
| `m 1`   | One-shot waveform player on slots 0–7            |

Switching mode hard-stops active voices. Wave sample data in AXI is kept
while powered; it is lost on reset (re-upload over USB CDC).

### Waveform (wave mode)

Eight AXI banks × 32 KiB (16384 int16 LE samples max). Path:
wave → amp/env → LPF → CH1. Playback rate (samples/s) = pitch_Hz × 128;
filter tracking uses pitch = rate / 128.

| Command              | Meaning                                                       |
| -------------------- | ------------------------------------------------------------- |
| `w`                  | Query lengths / playing (`*` = playing)                       |
| `w0`…`w7`            | Query one slot                                                |
| `w0`…`w7 <rate>`     | Play/restart at `rate` samples/s (1…192000)                   |
| `w0`…`w7 0`          | Stop slot                                                     |
| `wl <slot> <nbytes>` | **USB CDC only.** Begin binary upload; `nbytes` even, 2…32768 |

`n0`…`n7` in wave mode start the same one-shot with
`rate = Hz × 128` and respect envelope sustain/release. End of wave
silences that slot even if the key is still held. `err:mode` if `w*`
while in notes mode; `err:usb` if `wl` over RS485; `err:empty` if no
samples loaded.

Wave upload framing is defined in §4 (USB CDC).

### Oscillator shape (global)

| Command        | Meaning                                           |
| -------------- | ------------------------------------------------- |
| `s`            | Sine                                              |
| `p <0.1..0.9>` | Pulse; argument is duty                           |
| `t <0.1..0.9>` | Triangle; argument is asymmetry (0.5 = symmetric) |

### Amplitude envelope

Each voice can have a multi-segment linear envelope: pairs of
`(end_amp, slope[±k])` then a final `release_slope[±k]`. Start of the first
segment is always 0. Last segment is release to 0.

| Command               | Meaning                                |
| --------------------- | -------------------------------------- |
| `en`                  | List which slots have a program        |
| `en <tokens…>`        | Program **all** 16 voices              |
| `en 0`                | Clear all voices (unprogrammed bypass) |
| `en0`…`enf`           | Query one voice                        |
| `en0`…`enf <tokens…>` | Program one voice                      |
| `en0`…`enf 0`         | Clear one voice (unprogrammed bypass)  |

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
| `ek`                          | Dump k for all voices                        |
| `ek <k>`                      | Set the same k on all segments of all voices |
| `ek0`…`ekf` / `ek0`…`ekf <k>` | Query / bulk-set one voice                   |

`k` is in **[−10, 10]**. Default **0** (no pitch effect). `ek` is a bulk
override; prefer `slope±k` on `en` for per-segment values. Query prints one
value when all segments share k, otherwise one k per segment.

### Digital low-pass filter (voices 0–7)

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
| `cpu`               | On, 16 voices, DMA-style probe       |
| `cpu N`             | On, N voices (1..16)                 |
| `cpu q` / `cpu q N` | Soft-queue style probe               |
| `cpu 0`             | Off; clear notes; LED chaser resumes |

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
c:m 1
c:w0 48000
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

ADC7-bit addresses used at init: ADC1 `0x4C`, ADC2 `0x4D`.

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

## 4. USB CDC protocol

Channel Card USB exposes **two** host-facing functions. Do not conflate them:

| Interface            | Role                                                                    |
| -------------------- | ----------------------------------------------------------------------- |
| **UAC2** (speaker)   | Isochronous audio into CH1 — OS volume / DAW stream. Not this document. |
| **CDC ACM** (serial) | Same ASCII console as RS485, plus binary wave upload (`wl`).            |

Effect Card also has CDC for its console; it has no wave bank / `wl`.

### CDC vs RS485 (console)

|                | RS485                                       | USB CDC (ACM)                                                                  |
| -------------- | ------------------------------------------- | ------------------------------------------------------------------------------ |
| Commands       | Same parsers                                | Same parsers                                                                   |
| Line framing   | Host `\r`; card `\r\n`                      | Same                                                                           |
| Address prefix | Useful on a shared bus (`c:` / `e:` / `*:`) | Optional; still accepted                                                       |
| Reply tag      | `[C] ` / `[E] `                             | None — body only                                                               |
| Echo           | Channel: off. Effect: `ec` (default off)    | Local keystroke echo on                                                        |
| Baud           | **460800 8N1** on the UART                  | Host may open any rate (e.g. 115200); TinyUSB CDC ignores line coding for data |
| `wl` upload    | Rejected (`err:usb`)                        | Allowed                                                                        |

Typical host path on macOS / Linux: Channel `cu.usbmodem*` / `ttyACM*`.
USB–UART adapters (`cu.usbserial*`, `ttyUSB*`) are the RS485 dongle — wrong
port for `wl`.

### Wave upload session (`wl`)

Binary payload rides on the **same** CDC pipe as ASCII. After `ok:ready`,
the console leaves line mode: CDC RX bytes go to `WaveUpload_Feed` only
(no echo, no `\r` line framing, no command parse).

Constraints:

- Slot **0…7**
- `nbytes` even, in **[2, 32768]** (max one bank)
- Samples are **int16 little-endian**, contiguous

**Console reply API (what the card writes):** exactly two success lines for
a full transfer — nothing in between, even if USB delivers the payload in
many `tud_cdc_read` / `WaveUpload_Feed` calls.

| When                                  | Card writes (`USB_CDC_WriteStr`) |
| ------------------------------------- | -------------------------------- |
| `wl` accepted                         | `ok:ready\r\n`                   |
| During the `nbytes` of sample data    | *(no console reply)*             |
| Last byte received and bank committed | `ok:wave <slot> <nsamp>\r\n`     |

There is no per-chunk / per-`Feed` ACK. Mid-payload console traffic would
serialize Full-Speed CDC and is intentionally omitted.

Sequence:

```text
Host →  wl 0 8192\r
Card →  ok:ready\r\n          ← console write #1
Host →  <8192 raw bytes>      ← opaque; card may Feed() many times
Card →  ok:wave 0 4096\r\n    ← console write #2 (nsamp = nbytes/2)
```

Errors you may see instead of `ok:ready` / `ok:wave`:

| Body         | Meaning                                          |
| ------------ | ------------------------------------------------ |
| `err:usb`    | `wl` sent on RS485                               |
| `err:syntax` | Bad `wl` line                                    |
| `err:range`  | Slot / size / concurrent upload / commit failure |

Notes:

1. Keep the CDC session open across slots when loading a bank; reopen cost
   dominates, not the 32 KiB transfer.
2. While waiting for `ok:wave`, keep reading the CDC RX path — do not
   discard pending replies.
3. Upload only loads AXI RAM. To hear it: `m 1`, then `w0 <rate>` (or
   `n0 <Hz>` in wave mode). Data survives mode switches until power-cycle.

Example (one slot, then play):

```text
wl 3 32768\r
ok:ready
<32768 bytes>
ok:wave 3 16384
m 1\r
ok: m 1
w3 48000\r
ok
```
---

## 5. Half-duplex and pitch text

On RS485, send one command, wait for the tagged reply, then send the
next. Do not pile commands while an ACK is still due.

Prefer `e:ec 0` unless you are deliberately testing echo.

For pitch, send the real frequency as text (`261.625565`), not a rounded
integer, if you care about octaves lining up.
