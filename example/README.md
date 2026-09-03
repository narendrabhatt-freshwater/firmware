# cmi_core command-line example

This standalone C++17 application uses only the public `<cmi/core.hpp>` API.
It connects to the hardware, loads scripts into all eight Channel voices,
optionally loads one WAV or raw sample, plays a note, and disconnects.

## Build from this repository

```sh
cmake -S example -B example/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build example/build --parallel
```

## List MIDI inputs

```sh
example/build/cmi_core_example --list-midi
```

## Play the oscillator

Pass `--script` once to load the same script into every voice:

```sh
example/build/cmi_core_example \
  --rs485 /dev/cu.usbserial-0001 \
  --cdc /dev/cu.usbmodemCHCARD1 \
  --script cmi_core/examples/vm/channel/attack_2ms.be \
  --voice 0 \
  --key 60 \
  --duration-ms 2000
```

Pass `--script` exactly eight times when each voice needs a different script.
The arguments map to voices `0` through `7` in command-line order.

## Play a WAV or raw sample

```sh
example/build/cmi_core_example \
  --rs485 /dev/cu.usbserial-0001 \
  --cdc /dev/cu.usbmodemCHCARD1 \
  --script cmi_core/examples/vm/channel/attack_2ms.be \
  --sample samples/piano_c4.wav \
  --sample-id 60 \
  --root-hz 261.625565 \
  --voice 0 \
  --key 60
```

For signed 16-bit little-endian raw input, add `--raw-rate HZ`. WAV input is
converted to mono and resampled automatically.

Use `--midi "EXACT PORT NAME"` to enable automatic MIDI input and
`--audio "EXACT DEVICE NAME"` to select the Channel USB audio device. When
`--audio` is omitted, `cmi_core` selects the first compatible device.

Run `example/build/cmi_core_example --help` for every option.

## Build against an installed package

```sh
cmake -S example -B example/build-installed \
  -DCMI_CORE_EXAMPLE_USE_INSTALLED=ON \
  -DCMAKE_PREFIX_PATH=/path/to/cmi_core/install
cmake --build example/build-installed --parallel
```
