# midi_host — MIDI keyboard → speakers or Channel Card

PC-side C++17 app: MIDI in (RtMidi), 16-voice FIFO note bank, equal-temperament
pitch (A4 = 440 Hz). Output is either Mac speakers (RtAudio) or the Channel
Card note bank over RS485 (`n0`…`nf`).

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

On Channel Card mode, session start sends `n0` (bypass + defaults), then
**`gain 1 6`** (−6 dB on CH1), then `n0`…`nf 0` to clear leftover tones.
Each note is one RS485 exchange: `c:nX <Hz> 0.5` or `c:nX 0` (same path as
`fw rs485`). Scale 0.5 keeps the wave visible with `gain 1 6`; chords may
soft-clip past a couple of notes.

Example (speakers):

```text
midi:   Launchkey MK4 37 MIDI Out
output: speakers (MacBook Pro Speakers @ 48000 Hz)
playing… (Enter to quit)
ON     slot=0  note=69  A4   freq=440.00 Hz  active=1/16
```
