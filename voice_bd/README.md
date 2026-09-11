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

Each note-on prints its MIDI key, note name, assigned voice, and nominal BODY
demand, for example `key=72 (C5) voice=0 required=96.00 samples/ms (nominal BODY)`.
The estimate uses the uploaded sample's C4 root and the card's 1/16× minimum
speed. Rates above 2× are halved repeatedly to preserve the note's pitch class
within one octave above the sample root. The card also folds sample frequency
modulation above 2×. Custom firmware programs can change the actual demand.

Startup assumes the card is idle. If voices are active, program upload fails with
`err:vm-busy`; startup does not silence them or wait for their release.

For the reconnect experiment, startup sends `vq` immediately after opening RS485
and retries timeouts for up to three total attempts, with no pause between them.
Each attempt keeps the 50 ms reply timeout. No preliminary carriage return is
sent. Driver errors and invalid replies stop the attempts immediately. Normal
playback commands are not retried. Startup errors identify
the adapter path. Each startup attempt is printed as `vq attempt N/3`.
Timeouts include the received byte count; serial read failures report the driver error.

Input: 48 kHz, 16-bit PCM WAV, mono or stereo. Build requires RtAudio 5.2 or 6,
libserialport, CMake and pkg-config; other dependencies come from the repository.

The command-line parser uses the local `options.h` header.
Use `-h` to display the version, product identifier, and usage.

The voice board API returns `voice_board_result_t` with an error code and message.
The test program checks these results and prints failures before exiting.

## Replacing a playing sample

`load_sample()` can replace a sample while its notes are playing. It uploads the
new attack directly into card memory while USB audio, status polling and MIDI
continue. Once the upload and root-pitch command succeed, the host swaps its PCM
under the audio mutex. Old allocations are freed outside that mutex.

Each note keeps its next source position, measured in the full PCM sample.
Prepared packets and audio already queued on the card finish unchanged. A shorter
replacement continues if the position fits; otherwise the host stops sending
BODY data and sends the normal note-off. No envelope restart or replacement
crossfade is added. Attack and BODY updates are independent, so a brief audible
discontinuity is possible. The existing attack/BODY overlap remains unchanged.

The host retains the PCM prefix as well as the BODY so a replacement with a
longer attack can still supply any preserved source position. New notes start
BODY streaming at the replacement's attack-minus-overlap origin.

Use the updated channel firmware together with `voicebd`: older firmware rejects
attack uploads while bank memory is in use. Playback may read a mixture of old
and new attack bytes during upload. An incomplete upload leaves those bytes
partially overwritten with the previous length; a missing acknowledgement may
leave length completion unknown. Upload failure retains the host PCM. Errors after attack publication
identify partial completion; they do not imply rollback or close the device.

## Streaming protocol

Host implementation stays in `voicebd.h` and `voicebd.cpp`. The `vq` status
reply reports all eight playback durations and free-slot counts without a
status CRC. Pitch remains a card concern. USB Audio carries
1008 bytes/ms: a ten-byte header and up to 998 source samples for one or two
voices. The host requests `vq` every 5 ms. Its reply includes
one card-generated source-sample budget per voice for a 5 ms interval. The host
spreads that budget over USB milliseconds and keeps using the latest budget
through delayed polls; it never calculates playback pitch. Each new snapshot
corrects the allowance by replaying samples still in flight in USB order.
Free space is capped before each delivery, so an empty ring cannot earn
credit for consuming missing samples. The card reports how much audio time has passed since ingesting the acknowledged
USB packet. That places the snapshot on the host's USB packet timeline, without
using the RS485 round-trip time. Callback jitter does not advance that timeline.
The allowance accounts for the 1 ms USB and audio processing blocks. Voices
with different demands share packets to equalize buffered playback time. A stopped, pending, or mismatched session cannot
reuse the previous voice's budget. The bounded unacknowledged-packet ledger
still stops sends if acknowledgements disappear for too long.

The 61-byte `vq` reply is required; older firmware is rejected with an error.
Polling remains at 5 ms. Late replies do not add another full polling interval.
See `../channel_card/docs/protocol.md`.

Tests live separately in `tests/stream_test.cpp`; production sources contain no
self-test code or mock hooks. These tests cover the scheduler and status parser;
serial/audio integration requires hardware. Run without opening any device:

```sh
c++ -std=c++17 -Wall -Wextra -Werror \
  $(pkg-config --cflags rtaudio libserialport) tests/stream_test.cpp \
  $(pkg-config --libs rtaudio libserialport) -o /tmp/voicebd-stream-test
/tmp/voicebd-stream-test
```

The hardware reliability test also accepts mode `swaps` (after WAV and BEC
arguments). It repeatedly replaces a shared sample with equal, shorter and
64-sample versions while other voices play, then retriggers to verify recovery.
Run it only after installing the updated channel firmware; listen for persistent
noise or stuck notes and inspect card diagnostics. Brief swap clicks are allowed.

Hardware acceptance still requires measuring actual USB/poll timing and checking
zero `hold`/overflow faults under the intended workload. The scheduler cannot
guarantee uninterrupted audio above 998 source samples/ms or during excessive
stalls or sudden card-side speed increases.

`open()` reads initial voice timing and free space before loading programs.
`channel.bec` contains the Berry note program.
Each RS485 command is sent once and returns as soon as its complete reply
arrives. A missing reply times out after 50 ms; commands are not retried.
The poll cadence is separate from this reply deadline. Delayed
replies stretch the actual poll interval; USB keeps using its existing credit.

On macOS the RS485 port requests low receive latency with `IOSSDATALAT` so
short replies are not held in the serial driver's receive buffer. `open()`
establishes the USB Audio stream before notes are played. Note-on sends
`nX on <sample> <key> <velocity> @<session>` as one command. After its ACK,
USB uses the last confirmed free-space credit to begin filling the new note.
The first full packet goes to the new note. Starting with the second packet,
playing voices close to empty take precedence; otherwise startup priority
continues while the new note is pending. Once the card confirms playback,
the target is its five-ms sample demand (at least 998 samples), limited by ring
capacity minus the USB/audio phase allowance. After the first packet, playing voices close to empty
receive enough samples for the next packet and the two processing blocks first;
the new voice receives the remaining packet space. Priority ends once its
predicted buffered samples reach the target. This uses card demand, not host pitch. The card keeps the note pending until 998 samples
arrive, then starts ATTACK at an audio boundary. Split blocks accumulate
toward 998 samples. USB delivery latency therefore adds to note onset.
Note-on does not issue an extra `vq`; scheduled polls run between commands
when due.

The binary `vq` reply is 61 bytes. Each voice has a one-byte session,
five bytes packing an exact 13-bit free-space count, a 12-bit refill budget,
and a 15-bit remaining duration in 0.1 ms units. The card ring holds 4080
source samples per voice (85 ms at root pitch). Both card and host must use
the type-0x0C packed reply.
Duration rounds down, so quantization makes a deadline at most 0.1 ms earlier.
At 921600 baud the reply occupies about 0.662 ms on the UART; USB-driver and
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
