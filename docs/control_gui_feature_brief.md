# Freshwater Control — Feature Brief

Standalone product brief for designing a desktop control app for the Freshwater CMI hardware system.

---

## 1. Product

Freshwater Control is a desktop application that controls two audio hardware cards:

| Hardware         | Role                                                                       |
| ---------------- | -------------------------------------------------------------------------- |
| **Channel Card** | 16-voice digital synthesizer / sample player → main DAC output             |
| **Effect Card**  | Phantom power, audio enable, status LEDs, multi-channel USB mic / ADC path |

The operator connects once, then performs, sculpts sound, loads samples, and manages stage I/O from one app.

### Connections the app uses

| Link                                    | Purpose                                                              |
| --------------------------------------- | -------------------------------------------------------------------- |
| **RS485 serial** (921600 baud)          | Notes, tone, gain, envelopes, filters, effect controls, lab commands |
| **USB CDC** (separate Channel USB port) | Sample-wave bank upload only                                         |
| **MIDI** (optional)                     | Keyboard / controller → 16-voice allocator                           |
| **Local speakers** (optional)           | Host-side audio preview without relying on the card                  |

---

## 2. Users

| User                         | Needs                                                               |
| ---------------------------- | ------------------------------------------------------------------- |
| Musician / sound designer    | Play, shape tone, edit envelopes, load waves, hear result           |
| Hardware / bring-up engineer | Connect ports, recover bus faults, raw console, ADC register access |
| Demo / stage                 | Fast connect, obvious health, one-key silence, low clutter          |

---

## 3. Feature areas

The product is organized into six feature areas. Navigation and chrome are up to the designer; these are the capabilities that must exist.

1. **Perform** — live playing, voice overview, signal preview  
2. **Tone** — oscillator, filter, envelopes  
3. **Waves** — 8-slot sample bank upload and playback  
4. **Effect** — Effect-card power, LEDs, USB ADC, register tools  
5. **Lab** — raw protocol console  
6. **Setup** — connections, MIDI, output routing  

---

## 4. Cross-cutting features

These apply across the whole app.

### Connection health

Always visible:

| Status        | States                                  |
| ------------- | --------------------------------------- |
| Bus (RS485)   | Offline · Connecting · Online · Fault   |
| MIDI          | Off · On (with port identity available) |
| Playback mode | Notes · Wave                            |
| Wave upload   | Idle · Uploading                        |

### Output / safety controls

Always reachable (not buried in a submenu):

| Control     | Behavior                                                       |
| ----------- | -------------------------------------------------------------- |
| **Gain**    | 0–127 dB DAC attenuation. **0 = loudest** (label this clearly) |
| **Silence** | Immediate all-notes-off locally and on the card                |
| **Recover** | Clear RS485 fault / halt and resume communication              |

### Bus fault

When the serial link times out or note transmission stops:

- Highly visible fault indication  
- One-click recover  
- No silent “I clicked but nothing happened” failures  

### Activity log

Transcript of device replies and host messages:

- Success vs error visually distinct  
- Filter / search  
- Clear  
- Copy line  
- Collapsible so it does not permanently steal screen space  

### Notifications

Short transient feedback for connect results, failed commands, upload events, copy confirmation. Errors stronger / longer than successes.

### Persistence

Remember between launches: last ports, gain, output mode, playback mode, UI preferences the designer chooses to persist (e.g. panel sizes, last feature area).

### Keyboard performance shortcuts

| Input                     | Action                       |
| ------------------------- | ---------------------------- |
| Number keys or equivalent | Jump between feature areas   |
| Space                     | Silence                      |
| Letter row (piano map)    | Play notes when in Perform   |
| Octave keys               | Shift piano octave up / down |
| Help key                  | Show shortcut cheat-sheet    |

Exact key bindings can be refined; the capability must exist.

---

## 5. Perform

Live surface: is sound happening, which voices are on, play without leaving the screen.

### Oscilloscope preview (primary visual)

Host-side mixed preview of active voices (local sine mix for “what it should sound like” — not a bit-exact hardware DAC probe). Label it as a **local preview / scope**, not “Channel DSP output.”

Must behave like a real oscilloscope display:

| Control                         | Behavior                                                                                                |
| ------------------------------- | ------------------------------------------------------------------------------------------------------- |
| **Horizontal scale (timebase)** | Zoom time axis in/out (wider window ↔ more detail). User-adjustable; feel like a scope time/div control |
| **Vertical scale (volts/div)**  | Zoom amplitude axis in/out. User-adjustable; feel like a scope volts/div control                        |
| **Waveform**                    | Continuous glow-style trace of the mixed preview                                                        |
| **Readouts**                    | Peak, RMS, peak-to-peak, active voice count, window duration, sample rate (48 kHz)                      |
| **Level meter**                 | Amplitude meter with peak-hold                                                                          |

Horizontal and vertical scaling are first-class features, not fixed decorative sizes.

### Voices

- 16 slots labeled `n0` … `nf`  
- Show active vs silent; when active, show note name (and frequency in detail)  
- Select a voice to inspect  
- Jump from selected voice into Tone (envelope) or Waves  

### Play input

- On-screen piano  
- Computer-keyboard piano  
- MIDI controller  
- Octave shift  

Press = note on, release = note off, into the 16-voice allocator.

---

## 6. Tone

Sound design for the Channel card in **Notes** mode. Applying parameters requires the bus connected; when disconnected, show a clear empty state with a path to Setup — not dead grey controls with no explanation.

### Playback mode

| Mode      | Meaning                                                     |
| --------- | ----------------------------------------------------------- |
| **Notes** | Oscillators + envelopes + filter                            |
| **Wave**  | One-shot sample banks (sample management lives under Waves) |

### Oscillator (Notes mode)

| Control            | Detail                                                |
| ------------------ | ----------------------------------------------------- |
| Shape              | Sine · Pulse · Triangle                               |
| Pulse duty         | 0.1–0.9                                               |
| Triangle asymmetry | 0.1–0.9 (0.5 = symmetric)                             |
| Apply              | Commit shape to the card (global for all note voices) |

In Wave mode, oscillator controls are not applicable — explain and point to Waves.

### Digital low-pass filter

Applies to voices **n0–n7 only** (voices 8–15 have no filter path).

| Control               | Detail                                        |
| --------------------- | --------------------------------------------- |
| Voice select          | Among filterable voices                       |
| Bypass                | Full bypass (wide-open cutoff)                |
| Cutoff                | 20 Hz–20 kHz (log feel)                       |
| Q                     | 0.5–10                                        |
| Pitch tracking (`fk`) | 0–10; cutoff tracks note pitch relative to C4 |
| Apply                 | To selected voice or to all n0–n7             |

### Amplitude envelope

Multi-segment linear envelope, **2–10 segments including release**, per voice (or apply to all 16).

| Capability            | Detail                                                   |
| --------------------- | -------------------------------------------------------- |
| Interactive curve     | Breakpoints; drag; add splits                            |
| Segment parameters    | End amplitude, slope, pitch-track `k`, duration estimate |
| Factory shapes        | Pluck, Pad, Organ, Snappy (or equivalent starters)       |
| Add / remove segments | Within min/max                                           |
| Pitch-track preview   | How rate changes at C3 / C4 / C5 for a chosen `k`        |
| Undo                  | Local edit history                                       |
| Copy to all voices    | Mirror current program to all 16                         |
| Named presets         | Save / load user presets                                 |
| Apply / query         | Push program to card; read back from card                |

Also: small overview of all 16 voices’ envelope shapes for quick selection.

---

## 7. Waves

Manage eight sample banks (`w0`–`w7`) on the Channel card.

**Important:** Uploads use a **separate USB CDC** port from the RS485 control bus. The UI must make that distinction obvious.

### Bank operations

| Feature                          | Detail                                                        |
| -------------------------------- | ------------------------------------------------------------- |
| CDC port selection               | Enumerate serial devices; choose Channel USB modem port       |
| Playback rate                    | Roughly 1 kHz–96 kHz                                          |
| Load folder                      | Auto-assign files named for each slot (`w0_…raw` … `w7_…raw`) |
| Assign one file to all slots     | Batch assign                                                  |
| Per-slot browse / clear / upload | Independent slots                                             |
| Upload all                       | Queue all assigned slots                                      |
| Cancel upload                    | Stop background transfer                                      |
| Drag-and-drop files              | Onto the bank                                                 |
| Waveform thumbnail               | Per loaded slot                                               |
| Progress                         | Per slot and overall                                          |
| Play / Stop                      | Trigger card one-shot playback (requires bus + Wave mode)     |

Slot states: empty · assigned · queued · uploading · done · failed.

---

## 8. Effect

Controls for the Effect card on the shared RS485 bus.

**State honesty:** Until live hardware polling exists, toggles reflect **last command sent**. Provide an explicit **Query status** action so the user can refresh from the card. Do not imply continuous live telemetry unless labeled otherwise.

### Power / audio

- Query status  
- 48V phantom on/off  
- Audio enable on/off  
- RS485 echo on/off — **must stay off** during normal MIDI / Control use (warn if enabled)  

### LEDs

- Auto flash on/off  
- Red on/off  
- Yellow on/off  

### USB ADC channel

- Select which of 8 ADC channels streams over USB audio  
- Query current selection  

### ADC / I2C lab tools

- Read / write ADC registers (chip, register, value)  
- I2C scan  
- ADC init  

---

## 9. Lab

Power-user / bring-up console over the same serial protocol.

| Feature             | Detail                                                               |
| ------------------- | -------------------------------------------------------------------- |
| Target              | Channel · Effect · All (broadcast)                                   |
| Free command line   | Send arbitrary ASCII console commands                                |
| History             | Browse previous commands                                             |
| Convenience actions | Channel help, CPU probe helpers, Effect status                       |
| Feedback            | Replies in the activity log; clear error if command cannot be queued |

---

## 10. Setup

All connection complexity lives here so Perform / Tone stay clean.

### RS485 bus

- Enumerate serial ports + refresh  
- Manual path if needed  
- Connect / disconnect  
- Non-blocking connecting state  
- Reconnect last  
- Optional auto-reconnect on launch  
- Health counters (timeouts / errors)  

### MIDI

- Port list (plus sensible auto-pick option)  
- Open / close  
- Refresh  

### Output routing

| Mode     | Behavior                      |
| -------- | ----------------------------- |
| Speakers | Local host audio only         |
| Card     | Notes go to hardware only     |
| Both     | Hear locally and send to card |

### Wave USB hint

Explain that wave upload needs the separate Channel USB CDC port; deep-link into Waves.

---

## 11. Domain rules (label accurately)

| Topic            | Rule                                                                         |
| ---------------- | ---------------------------------------------------------------------------- |
| Voices           | 16 slots: `n0` … `nf`                                                        |
| Frequency        | Fractional Hz; valid set range roughly 20–19999 Hz; 0 = off                  |
| Gain             | 0–127 dB atten; **0 = loudest**                                              |
| Filter           | Voices 0–7 only                                                              |
| Envelope         | 2–10 segments including release; per-segment slope and pitch-track `k`       |
| Notes vs Wave    | Two exclusive playback modes on the Channel card                             |
| Protocol replies | Success `ok` / `ok: …`; errors `err:…`; faults halt further TX until Recover |

---

## 12. States that need explicit design

| State                | Requirement                                  |
| -------------------- | -------------------------------------------- |
| Nothing connected    | Calm empty states; Setup as the path forward |
| Connecting           | Non-blocking; visible progress/state         |
| Online               | Controls enabled; status clear               |
| Bus fault            | Banner + Recover; no silent drops            |
| Speakers / MIDI only | Perform still useful without the card        |
| Upload in progress   | Progress + cancel; UI stays responsive       |
| Feature needs bus    | Explicit “not connected” with Setup CTA      |
| Effect toggles       | Labeled as last-sent; Query is explicit      |

---

## 13. Platform & constraints

- Desktop: macOS, Linux, Windows  
- Mouse + keyboard primary; performance keyboard path required  
- Resizable window; must work on laptop heights  
- High-contrast Offline / Online / Fault  
- Not mobile / tablet / web for this product  

---

## 14. Out of scope (do not invent)

- Continuous live hardware state sync (future firmware work)  
- Extra DAC channel tone/DC editors not exposed on the wire protocol  
- Full DAW arrange view, automation, plugin hosting  
- Multi-rig fleets beyond one Channel + one Effect on one bus  

---

## 15. One-line summary

A desktop instrument control surface for a 16-voice Channel card and an Effect I/O card: connect in Setup, perform and sculpt in Perform / Tone / Waves (with a true horizontally and vertically scalable oscilloscope preview), manage stage I/O in Effect, drop to Lab when the wire matters — always with unmistakable link health and one-key silence.
