# CMI Control (Dear ImGui)

Cross-platform desktop GUI and example application for MIDI + RS485 console
control of Channel and Effect cards. It uses the same ASCII protocol available
from any serial terminal.

## Framework

| Layer              | Choice                                                                        |
| ------------------ | ----------------------------------------------------------------------------- |
| UI                 | **Dear ImGui** 1.91                                                           |
| Window / input     | **GLFW** 3.4                                                                  |
| Graphics           | **OpenGL 3** (macOS Core Profile / Linux / Windows)                           |
| Host SDK           | [`protocol`](../protocol): card protocol, MIDI, audio, RS485, USB |

Runs on **macOS, Linux, and Windows** with CMake + a C++17 toolchain.
Dependencies are pulled by FetchContent on first configure (needs network once).

## Build / run

```bash
fw control build
fw control

# or:
cmake -S cmi_control -B cmi_control/build
cmake --build cmi_control/build
./cmi_control/build/cmi_control
```

### Host packages (typical)

| OS      | Notes                                                                                         |
| ------- | --------------------------------------------------------------------------------------------- |
| macOS   | Xcode CLI tools; OpenGL and CoreAudio via system frameworks                                  |
| Linux   | `build-essential cmake`, GLFW deps (`libgl1-mesa-dev` `libx11-dev` …), ALSA/Pulse for RtAudio |
| Windows | MSVC or MinGW; OpenGL from system                                                             |

## Views

1. **Perform** — full-height preview scope with TIME/DIV / VOLTS/DIV controls, voice grid, two-octave on-screen + computer keyboard piano (A–K / W E T Y U, Z/X octave)
2. **Tone** — oscillator shape with waveform preview, n0–n7 LPF + fk, multi-segment envelope editor (voice thumbnails, presets, undo, segment table)
3. **SAMPLE** — 8-voice sample set, CDC attack upload (`al`), 10-channel int16 UAC2 BODY streaming, drag-and-drop files, root pitch (`ar`), voice assignment (`aw`)
4. **Effect** — phantom / AUDIO_EN / LEDs (last-sent state), USB ADC channel, ADC/I2C tools

## Channel Berry program test

The Tone page contains a **CHANNEL BERRY PROGRAM** panel with three deliberately
small examples: Envelope, Gate, and Pluck. Select the Channel Card CDC port,
choose a program, and press **BUILD + UPLOAD n0–n7**.

The build generates cached FWSC files under `cmi_control/build/vm_scripts`.
The GUI checks the selected source and cache again at upload time and recompiles
when the FWSC is missing, corrupt, or older than its `.be` source. It then sends
note-off to every voice, waits for release, and uploads the same program to all
eight voice slots over USB CDC. Keep the card idle during this initial test;
the operation intentionally silences any playing notes.
5. **Setup** — RS485 / MIDI / output routing, auto-reconnect, port refresh

The UI follows a phosphor-terminal design language (green-on-dark,
scanline accents).

## Shell

- Left 56 px icon rail: view switcher, Recover (on fault), shortcuts
- Top status bar: BUS / MIDI chips with inline connect popovers, VOICES count, gain, Silence
- Right LOG + CONSOLE panel (collapsible): timestamped log with filter, raw command line with CH/EF/ALL target tabs, ↑↓ history, quick sends
- Bus-fault banner with Recover under the status bar
- Layout and ports persisted to `~/Library/Application Support/CMI/control_gui.ini` (macOS) or `~/.config/cmi/` on Linux
- About: **F1**

## Shortcuts

| Key                  | Action                  |
| -------------------- | ----------------------- |
| `1`–`5`              | Switch views            |
| `Space`              | All notes off / silence |
| `Cmd/Ctrl` `+`/`-`/`0` | UI zoom in / out / reset (persisted) |
| `F1`                 | About                   |
| `?`                  | Shortcuts overlay       |
| `A`–`K`, `W E T Y U` | Piano (Perform)         |
| `Z` / `X`            | Octave down / up        |

Channel envelope programs receive the physical MIDI key and use two-argument
`ramp(target, slope)`. A script can calculate pitch-dependent slopes with
allocation-free `pow()` and inspect raw/mapped current or pending keys.

**Preview scope** applies the uploaded FWSC key map and tuning reference to its
illustrative sine mix; it is not a bit-exact Channel DSP or DAC probe.

## Output modes

| Mode     | Behavior                             |
| -------- | ------------------------------------ |
| Speakers | Local RtAudio only (no bus required) |
| Card     | RS485 note TX only                   |
| Both     | Local hear + card                    |

Effect echo is forced off at session open (MIDI / burst safe).
