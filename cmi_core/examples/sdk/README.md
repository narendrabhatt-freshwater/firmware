# cmi_core SDK example

This standalone C++17 application uses only the public `<cmi/core.hpp>` API.
It connects to the hardware, loads scripts into all eight Channel voices,
loads one WAV or raw sample, plays a note, and disconnects.

## Build from this repository

```sh
cmake -S cmi_core/examples/sdk -B build/sdk -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/sdk --parallel
```

## List MIDI inputs

```sh
build/sdk/cmi_core_example --list-midi
```

## Play a WAV or raw sample

```sh
build/sdk/cmi_core_example \
  --script cmi_core/examples/vm/channel/channel.be \
  --sample samples/piano_c4.wav \
  --sample-id 60 \
  --root-hz 261.625565 \
  --voice 0 \
  --key 60
```

For signed 8-bit raw input, add `--raw-rate HZ`. WAV input is
converted to mono and resampled automatically.

The RS485 adapter, Channel CDC port, and audio device are discovered when each
has one unique match. Use `--rs485`, `--cdc`, or `--audio` to resolve an
ambiguous setup.

Pass `--script` exactly eight times when each voice needs a different script.
The arguments map to voices `0` through `7` in command-line order.

Use `--midi "EXACT PORT NAME"` to enable automatic MIDI input and
`--audio "EXACT DEVICE NAME"` to select the Channel USB audio device. When
`--audio` is omitted, `cmi_core` selects the first compatible device.

Run `build/sdk/cmi_core_example --help` for every option.

## Build against an installed package

```sh
cmake -S cmi_core/examples/sdk -B build/sdk-installed \
  -DCMI_CORE_EXAMPLE_USE_INSTALLED=ON \
  -DCMAKE_PREFIX_PATH=/path/to/cmi_core/install
cmake --build build/sdk-installed --parallel
```
