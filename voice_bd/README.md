# voicebd

```sh
cd voice_bd
make
./voicebd piano.wav
```

An optional second argument selects the firmware program:
`./voicebd piano.wav /path/to/channel.bec`. If omitted, `channel.bec` is
read from the current working directory.

`make` produces `voicebd` and `channel.bec` in this folder. Intermediate
build files go in `.build`.

To use the standalone compiler in `175-mainframe/mas`, configure from this folder:

```sh
cmake -S . -B .build -DBERRY_EXECUTABLE="$PWD/../../175-mainframe/mas/berry"
make
```

The build runs `berry channel.be -o channel.bec`. The compiled program includes
the firmware header and checksum required by the uploader. External compilers
must emit this same format.

Set the three port names in `voice_board_config_t` in `voicebd.h` before building.
The program uses those names and MIDI input 0, loads the sample for all eight
voices, and reports board errors before exiting with a nonzero status. Ctrl+C stops playback and exits.

Startup assumes the card is idle. If voices are active, program upload fails with
`err:vm-busy`; startup does not silence them or wait for their release.

Input: 48 kHz, 16-bit PCM WAV, mono or stereo. Build requires RtAudio 5.2 or 6,
libserialport, CMake and pkg-config; other dependencies come from the repository.

The command-line parser uses the local `options.h` header.
Use `-h` to display the version, product identifier, and usage.

The voice board API returns `voice_board_result_t` with an error code and message.
The test program checks these results and prints failures before exiting.

## Streaming protocol

Host implementation stays in `voicebd.h` and `voicebd.cpp`. The `vq` status
reply reports all eight playback durations and free-slot counts without a
status CRC. Pitch remains a card concern. USB Audio carries
1008 bytes/ms: a ten-byte header and up to 998 source samples for one or two
voices. RS485 polls every 5 ms; USB keeps running
during serial transactions. Known free slots limit every send; acknowledged
sequences reconcile samples in flight. See `../channel_card/docs/protocol.md`.

Tests live separately in `tests/stream_test.cpp`; production sources contain no
self-test code or mock hooks. These tests cover the scheduler and status parser;
serial/audio integration requires hardware. Run without opening any device:

```sh
c++ -std=c++17 -Wall -Wextra -Werror \
  $(pkg-config --cflags rtaudio libserialport) tests/stream_test.cpp \
  $(pkg-config --libs rtaudio libserialport) -o /tmp/voicebd-stream-test
/tmp/voicebd-stream-test
```

Hardware acceptance still requires measuring actual USB/poll timing and checking
zero `hold`/overflow faults under the intended workload. The scheduler cannot
guarantee uninterrupted audio above 998 source samples/ms or during excessive
stalls or sudden card-side speed increases.

`open()` reads initial voice timing and free space before loading programs.
`channel.bec` contains the Berry note program.
Each RS485 command is sent once and returns as soon as its complete reply
arrives. A missing reply times out after 5 ms; commands are not retried.
The 5 ms poll cadence is separate from this reply deadline. Delayed
replies stretch the actual poll interval; USB keeps using its existing credit.

On macOS the RS485 port requests low receive latency with `IOSSDATALAT` so
short replies are not held in the serial driver's receive buffer. `open()`
establishes the USB Audio stream before notes are played. Note-on sends
`nX on <sample> <key> <velocity> @<session>` as one command. After its ACK,
USB uses the last confirmed free-space credit to prioritize the new note's
first 998 BODY samples. The card keeps the note pending until those samples
arrive, then starts ATTACK at an audio boundary. Split blocks accumulate
toward 998 samples. USB delivery latency therefore adds to note onset.
Note-on does not issue an extra `vq`; scheduled polls run between commands
when due.

The binary `vq` reply is 53 bytes. Each of eight voices has a one-byte session,
two-byte free-slot count, and two-byte remaining duration in 0.1 ms units.
Duration rounds down, so quantization makes a deadline at most 0.1 ms earlier.
At 921600 baud the reply occupies about 0.575 ms on the UART; USB-driver and
scheduling delays are additional. USB sample delivery continues during polling.

`open()` keeps the USB upload connection available until `close()`.
`load_sample()` converts and uploads at most 512 ATTACK samples, stores one copy
of the signed-16 PCM BODY, and sends the sample's root pitch. The packet-fill
loop converts only the BODY samples being transmitted to signed eight-bit.
The retained BODY uses two bytes per sample. Replacing it releases the previous
allocation outside the streaming-state lock.

Hardware workload test (no MIDI, unoptimized host):

```sh
c++ -O0 -g -std=c++17 -Wall -Wextra -Werror \
  $(pkg-config --cflags rtaudio libserialport) tests/reliability_test.cpp voicebd.cpp \
  $(pkg-config --libs rtaudio libserialport) -o /tmp/voicebd-reliability-test
/tmp/voicebd-reliability-test sample.wav test.bec burst
/tmp/voicebd-reliability-test sample.wav test.bec startups 60
/tmp/voicebd-reliability-test sample.wav test.bec steady 72
```

`burst` sends 4096 note-ons with interleaved note-offs; `startups` repeats 64
eight-voice starts/releases; `steady` holds eight voices for 30 seconds. The
optional key applies to `startups` and `steady`. Tests use 40 dB attenuation.
Inspect card `usb`, `vm mem`, and `fault` diagnostics as well as host exit status.
After a latched fault, use the reset button and read `fault` before clearing it.
