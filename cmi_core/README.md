# cmi_core production SDK

`cmi_core` is the C++17 host SDK for a Freshwater CMI. One `cmi::Core`
instance owns card communication, Channel Berry programs, sample playback,
USB BODY streaming, MIDI input, voice allocation, and supported Effect Card
controls. The package builds without downloading dependencies.

Certified release targets are macOS on Intel and Apple Silicon, Ubuntu
x86_64, and Ubuntu/Debian ARM64 on Rockchip. Other POSIX systems are
best-effort. Certification means the release has a recorded hardware run as
described in [RELEASE.md](RELEASE.md); until those records and production USB
identifiers exist, the version is a release candidate rather than production.

## 1. Prerequisites

Berry, RtMidi, and RtAudio are included. The host supplies CMake, Ninja, a
C++17 compiler, and its native audio/MIDI services.

macOS requires Xcode Command Line Tools, CMake, and Ninja:

```sh
xcode-select --install
brew install cmake ninja
```

Ubuntu/Debian, including ARM64 Rockchip, requires:

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build libasound2-dev
sudo usermod -aG dialout "$USER"
```

After changing `dialout`, sign out and back in. The SDK never installs system
packages or changes permissions itself.

## 2. Build and diagnose

From the `cmi_core` directory:

```sh
cmake --preset release
cmake --build --preset release
```

Connect the Channel Card and RS485 adapter, then run:

```sh
build/release/cmi-play doctor
```

`doctor` detects the host, RS485 adapter, Channel CDC port, compatible
21-channel audio output, and MIDI inputs. It then connects, queries the Channel
Card, and disconnects. Missing devices, Linux permissions, port ownership, and
ambiguous adapters are reported with corrective instructions.

## 3. Use the class

Only `<cmi/core.hpp>` is application-facing. Discovery fills empty fields but
never guesses when more than one required device matches:

```cpp
#include <cmi/core.hpp>
#include <iostream>

int main()
{
  cmi::SetupReport setup = cmi::Core::discover();
  if (!setup.ready()) {
    for (const auto &message : setup.diagnostics)
      std::cerr << message << '\n';
    return 1;
  }

  cmi::Core core(setup.params);
  core.setErrorHandler([](const cmi::Result &error) {
    std::cerr << error.message << '\n';
  });
  if (!core.connect()) return 1;
  if (!core.loadVoiceScriptAll("examples/vm/channel/channel.be")) return 1;

  cmi::SampleDefinition sample;
  sample.id = 60;
  sample.sample_file = "/path/to/my/piano_c4.wav";
  sample.root_hz = 261.625565;
  if (!core.loadSample(sample)) return 1;
  if (!core.sampleNoteOn(0, 60, sample.id)) return 1;
  return core.disconnect() ? 0 : 1;
}
```

Pass explicit `CoreParams` values before discovery to select devices in a
multi-device system. Explicit values are preserved. Set
`DiscoveryOptions::select_midi` or `require_audio` to false when that facility
is intentionally unused.

Calls wait for their card reply and return `cmi::Result`. Run connection,
script upload, and sample loading on an application worker thread. Register an
error handler for asynchronous MIDI and BODY-stream failures.

## 4. Run a user project

The installed `cmi-play` executable is a reference runner and support tool. It
does not ship or synthesize samples or wavetables. A project contains paths and
assets owned by the user:

```text
my-sound/
├── channel.be
├── samples/
│   └── w60_piano.wav
└── wavetables/
    └── osc0.wav
```

Run it with automatic device discovery:

```sh
build/release/cmi-play my-sound
```

Or load one file quickly:

```sh
build/release/cmi-play my-sound \
  --sample /path/to/piano_c4.wav --sample-id 60 --root-hz 261.625565
```

Use `--rs485`, `--cdc`, `--audio`, or `--midi` to resolve ambiguity. Only
assets found at the supplied paths are uploaded. Each upload stage prints its
elapsed time. Saving `channel.be` reloads it into all eight voices.

## 5. Samples and scripts

Combined WAV files are converted to mono and resampled to 48 kHz. Combined raw
files are signed 8-bit; set `raw_sample_rate_hz` when necessary. Pre-split
signed-int8 attack/BODY pairs and folders using `wN_*_head.i8`,
`wN_*_body.i8`, and `roots.txt` are also supported. Sample IDs are `0..247`;
`248..255` are reserved for oscillator tables.

The shipped [channel.be](examples/vm/channel/channel.be) is sample-only by
default. Oscillator examples remain commented until the application uploads a
user wavetable with `loadWavetable(0..7, path)`.

See [SCRIPTING.md](SCRIPTING.md) for the Berry language and limits, and
[PROTOCOL.md](PROTOCOL.md) for the wire protocol.

## 6. Consume an installed SDK

```cmake
find_package(cmi_core CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE cmi::core)
```

Install and verify a real consumer with:

```sh
cmake --install build/release --prefix /path/to/install
cmake -S examples/sdk -B build/sdk \
  -DCMI_CORE_EXAMPLE_USE_INSTALLED=ON \
  -DCMAKE_PREFIX_PATH=/path/to/install
cmake --build build/sdk --parallel
```

For in-tree use, call `add_subdirectory(path/to/cmi_core)` and link
`cmi::core`. Set `CMI_CORE_BUILD_CLI=OFF` when embedding the SDK without its
reference executable.

## 7. Troubleshooting

- **No RS485 adapter:** confirm it creates `cu.usbserial*`/`ttyUSB*`, supports
  921600 baud, or set `CoreParams::rs485_port` explicitly.
- **No Channel CDC:** ensure application firmware—not DFU—is running and look
  for `cu.usbmodem*`/`ttyACM*`.
- **Permission denied on Linux:** add the user to `dialout`, sign out, and back
  in; also check distribution-specific udev policy.
- **Device busy:** close serial monitors and other CMI applications. The SDK
  requests exclusive ownership to prevent reply corruption.
- **No compatible audio:** the Channel firmware must expose its class-compliant
  21-channel, signed-int8, 48 kHz output.
- **Multiple matches:** inspect `SetupReport` or `cmi-play doctor`, then provide
  the exact override. Discovery deliberately does not choose arbitrarily.
- **Upload failure:** verify the CDC path, firmware/protocol version, file
  format, and sample ID. The returned result includes the card reply when one
  was received.
- **Timeout after reconnect:** call `disconnect()`, reconnect the hardware,
  call `connect()`, then reload RAM-resident scripts and samples.

Run all software qualification checks with `scripts/qualify.sh`. Hardware
release requirements and performance records are defined in
[RELEASE.md](RELEASE.md).
