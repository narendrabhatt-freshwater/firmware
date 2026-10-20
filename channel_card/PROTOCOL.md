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
| RS485 `vq` | Fixed 56-byte binary status, without a text tag |

Examples of bodies (tag omitted for clarity):

```text
ok
ok:vm mask 01
err:syntax
err:range
err:unknown
```

Note events have no reply. Gain success is `ok`. Query commands usually
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
before acknowledging clear, retaining loaded samples/programs and USB link state.

## 3. USB protocol

This branch changes the shared application protocol and matching desktop client.
STM32 hardware integration and timing have not been qualified for this change.
The short command list is in [serial_commands.md](../serial_commands.md).

USB is an ordered binary byte stream. Read/write boundaries do not delimit
messages. All multibyte integers below are little-endian. Open the connection
in raw mode; there is no initial HELLO exchange.

### BODY

| Offset | Bytes | Field |
|---|---:|---|
| 0 | 1 | Command 1 |
| 1 | 1 | Voice 0..7 |
| 2 | 1 | Session 0..254 |
| 3 | 2 | Payload length, 1..1024 |
| 5 | length | Signed 8-bit PCM samples |

BODY never receives a USB reply. Malformed fields or a partial frame stalled
for over one second still fault reception. Complete valid frames for obsolete
sessions are discarded by the existing session check. A frame for a live
session waits in the parser until enough stream space is available.

Current and pending notes share a 4080-sample pool per voice, allocated in
16-sample blocks. The host reserves space for all unprocessed BODY frames,
including older sessions, and reconciles it using `vq`.
An idle note retains its 998-sample prime threshold. An active note's Berry
program controls when its replacement starts. The host continues supplying
the outgoing session through that transition and through release tails.

### UPLOAD

| Offset | Bytes | Field |
|---|---:|---|
| 0 | 1 | Command 2 |
| 1 | 1 | Kind |
| 2 | 1 | Target |
| 3 | 4 | Total data bytes |
| 7 | total | Consecutive data bytes |

| Kind | Target | Total size |
|---|---|---|
| 1 attack | sample 0..247 | 1..512 bytes |
| 2 wavetable | logical wave 0..7 | 2..512 bytes |
| 3 program | voice 0..7 | 20..16404 bytes, FWSC ABI2 |

Send one header and all of its data. The receiver tracks progress internally;
there are no BEGIN, DATA, offset or ABORT messages. It processes input as bytes
arrive, so a program need not fit in the input queue. Do not interleave BODY
inside an upload. There is one upload reply, after all data has arrived and
validation/commit has finished. Only one upload request may be outstanding.

Success reply: `target:u8, command:02, status:00`.
Error reply: `target:u8, command:02, status:01, error_text, LF:0A`.
There is no separate REPLY command or success payload length. Error text has
no embedded newline and is at most 95 bytes before its LF terminator.
A known-length rejected upload is consumed to its declared end before replying.
Zero or over-limit total sizes fault the stream and return an error immediately.
An upload stalled for over five seconds is aborted and faults reception, since
remaining data cannot be interpreted as a new command header.

Attack bytes overwrite live storage as received; an interrupted upload does
not roll them back. Committed length changes only after completion. Wavetable
uploads retain their existing bank-use restrictions. Invalid or interrupted
program replacements preserve the selected program. Reopening clears partial
parser state, queued replies and unfinished uploads.

### Voice queue status (`c:vq`)

The reply has 56 bytes, including framing:

```text
0..1    a5 5a
2       43 ('C')
3       0f (status format)
4       active voice mask
5..6    processed BODY sequence u16
7..54   eight six-byte records:
          voice u8, playing session u8, free samples u16, five-ms demand u16
55      0a
```

Session 255 means no playing stream. The active mask describes sounding voices;
the client tracks the replacement notes it sends. There is no pending mask,
second session, capacity, BODY age or remaining-time field on the wire.

The sequence counts processed BODY frames across all voices, including obsolete
sessions, and wraps at 65536. It is not a per-status counter. Uploads and BODY
frames retained for space do not advance it. Free space and sequence come from
the same snapshot. The host subtracts samples still in transit from reported
free space before sending more. It derives a conservative coverage estimate
from occupancy and demand, excluding data prepared for a pending replacement.

After opening USB and completing program uploads, the host reads a fresh `vq`
to establish the sequence baseline before streaming. Note clearing and USB
reopening do not reset the processed BODY count. Application initialization does.
Use matching host and application builds; the older 61-byte status is incompatible.

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

For RS485 commands that reply, finish that exchange before starting another.
Note events do not reply and can be sent consecutively.

Keep other devices' keystroke echo disabled on the shared RS485 bus.

For notes, send the physical MIDI key as an integer from 0 through 127. The
loaded FWSC program owns mapping and tuning.
