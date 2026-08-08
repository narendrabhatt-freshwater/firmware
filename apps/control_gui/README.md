# CMI Control (Dear ImGui)

Cross-platform desktop GUI for MIDI + RS485 console control of Channel and
Effect cards. Same ASCII protocol as `apps/console` / any serial terminal;
voice allocation matches `apps/midi_host`.

## Framework

| Layer              | Choice                                                                        |
| ------------------ | ----------------------------------------------------------------------------- |
| UI                 | **Dear ImGui** 1.91                                                           |
| Window / input     | **GLFW** 3.4                                                                  |
| Graphics           | **OpenGL 3** (macOS Core Profile / Linux / Windows)                           |
| MIDI / local audio | RtMidi + RtAudio (same as `midi_host`)                                        |
| Bus                | [`libs/host_io`](../../libs/host_io) + [`libs/protocol`](../../libs/protocol) |

Runs on **macOS, Linux, and Windows** with CMake + a C++17 toolchain.
Dependencies are pulled by FetchContent on first configure (needs network once).

## Build / run

```bash
fw control build
fw control

# or:
cmake -S apps/control_gui -B apps/control_gui/build
cmake --build apps/control_gui/build
./apps/control_gui/build/control_gui
```

### Host packages (typical)

| OS      | Notes                                                                                         |
| ------- | --------------------------------------------------------------------------------------------- |
| macOS   | Xcode CLI tools; OpenGL via system frameworks                                                 |
| Linux   | `build-essential cmake`, GLFW deps (`libgl1-mesa-dev` `libx11-dev` …), ALSA/Pulse for RtAudio |
| Windows | MSVC or MinGW; OpenGL from system                                                             |

## Views

1. **Perform** — full-height preview scope with TIME/DIV / VOLTS/DIV controls, 4×4 voice grid, two-octave on-screen + computer keyboard piano (A–K / W E T Y U, Z/X octave)
2. **Tone** — playback-mode strip, oscillator shape with waveform preview, n0–n7 LPF + fk, multi-segment envelope editor (voice thumbnails, presets, undo, segment table)
3. **Waves** — 8-slot bank in a 4-column grid, CDC upload, drag-and-drop files, waveform thumbnails, per-slot playback rate
4. **Effect** — phantom / AUDIO_EN / LEDs (last-sent state), USB ADC channel, ADC/I2C tools
5. **Setup** — RS485 / MIDI / output routing, auto-reconnect, port refresh

The UI follows the phosphor-terminal design in
`vite/Freshwater Control App Design` (React reference implementation).

## Shell

- Left 56 px icon rail: view switcher, Recover (on fault), shortcuts
- Top status bar: BUS / MIDI chips with inline connect popovers, MODE toggle, VOICES count, gain, Silence
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

Envelope programs match firmware: odd token lists, `slope±k` glued (e.g. `10+1`, `2.0-0.5`), 2–10 segments including release.

**Preview scope** is a local sine mix for “what it should look like,” not a
bit-exact Channel DSP or DAC probe.

## Output modes

| Mode     | Behavior                             |
| -------- | ------------------------------------ |
| Speakers | Local RtAudio only (no bus required) |
| Card     | RS485 note TX only                   |
| Both     | Local hear + card                    |

Effect echo is forced off at session open (MIDI / burst safe).
