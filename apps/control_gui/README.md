# control_gui — Freshwater Control (Dear ImGui)

Cross-platform desktop GUI for MIDI + RS485 console control of Channel and
Effect cards. Same ASCII protocol as `apps/console` / any serial terminal;
voice allocation matches `apps/midi_host`.

## Framework

| Layer | Choice |
| --- | --- |
| UI | **Dear ImGui** 1.91 |
| Window / input | **GLFW** 3.4 |
| Graphics | **OpenGL 3** (macOS Core Profile / Linux / Windows) |
| MIDI / local audio | RtMidi + RtAudio (same as `midi_host`) |
| Bus | [`libs/rs485`](../../libs/rs485) |

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

| OS | Notes |
| --- | --- |
| macOS | Xcode CLI tools; OpenGL via system frameworks |
| Linux | `build-essential cmake`, GLFW deps (`libgl1-mesa-dev` `libx11-dev` …), ALSA/Pulse for RtAudio |
| Windows | MSVC or MinGW; OpenGL from system |

## Views

1. **Perform** — MIDI / on-screen keys, 16 voice meters, Preview scope, Speakers/Card/Both
2. **Tone** — oscillator shape, LPF + fk, **multi-segment envelope editor** (per-segment end amp, slope, pitch-track k, live curve, ASCII `en`/`enN`)
3. **Effect** — phantom, AUDIO_EN, LEDs, USB ADC channel, echo, I2C/ADC
4. **Lab** — raw command line + CPU helpers

Envelope programs match firmware: odd token lists, `slope±k` glued (e.g. `10+1`, `2.0-0.5`), 2–10 segments including release.

**Preview scope** is a local sine mix for “what it should look like,” not a
bit-exact Channel DSP or DAC probe.

## Output modes

| Mode | Behavior |
| --- | --- |
| Speakers | Local RtAudio only (no bus required) |
| Card | RS485 note TX only |
| Both | Local hear + card |

Effect echo is forced off at session open (MIDI / burst safe).
