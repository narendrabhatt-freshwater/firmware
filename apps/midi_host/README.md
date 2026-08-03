# midi_host — MIDI keyboard → speakers or Channel Card

PC-side C++17 app: MIDI in (RtMidi), 16-voice FIFO note bank on the **host**,
equal-temperament pitch (A4 = 440 Hz). Output is either Mac speakers (RtAudio)
or the Channel Card over RS485 — **one ASCII `nX` command per event** with
strict ACK (shared [`libs/rs485`](../../libs/rs485)).

## MIDI input — any controller

Auto-pick prefers a Novation **Launchkey “… MIDI Out”** port (skips DAW Out).
Any other keyboard works if you pass an index from `fw midi list`:

```bash
fw midi list
fw midi --midi 0          # any device at index 0 → speakers
```

## Output — speakers vs Channel Card

| Command | Where sound goes |
| --- | --- |
| `fw midi` | Mac speakers (default) |
| `fw midi channel --rs485 PATH --gain 6` | Channel Card; CH1 atten −6 dB at start |
| `fw midi channel --rs485 PATH --midi 0 --gain 12` | same, MIDI port 0, −12 dB |

`--gain DB` is session-start only (`gain 1 <DB>` once). Default **6** (or `FW_MIDI_GAIN`).
`--port N` is an alias for `--midi N`. With `channel`, `--port /dev/...` means
the RS485 adapter. You can also set `FW_RS485_PORT`.

## Build

```bash
fw midi build
```

## Behaviour

| Event | Meaning |
| --- | --- |
| `ON` | New key allotted to a free slot |
| `OFF` | Key released; slot freed |
| `RETRIG` | Same key pressed again → same slot, phase reset |
| `STEAL` | Bank full (16); oldest note dropped before new `ON` |

Velocity is ignored. Pitch: \(f = 440 \times 2^{(n-69)/12}\).

### Channel Card RS485 session

1. `c:quiet off` (recover sticky quiet)
2. Effect echo per flag: `--echo-off` (default), `--echo-on`, or `--echo-leave`
3. `c:n0`, `c:gain 1 <dB>`, `c:silence`
4. Each On/Off/Retrig → one `c:nX <Hz>` or `c:nX 0`, wait for `[C]ok`
5. On quit: best-effort `silence` + `quiet off`

Voice allocation stays in host `VoiceBank`. Card default scale is **0.125**
when Hz is sent without a scale argument. No binary bank frames; no `quiet on`
on the production path. Timeouts trigger soft recovery (`silence`); a latched
bus fault stops further note TX until reopen.

RS485 defaults to **115200** baud. Same ASCII works from `screen` /
`rs485_console` — see [`docs/rs485_console_architecture.md`](../../docs/rs485_console_architecture.md).

Example (speakers):

```text
midi:   Launchkey MK4 37 MIDI Out
output: speakers (MacBook Pro Speakers @ 48000 Hz)
playing… (Enter to quit)
ON     slot=0  note=69  A4   freq=440.00 Hz  active=1/16
```
