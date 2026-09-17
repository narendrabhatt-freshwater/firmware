# voicebd

This folder builds independently; no sibling repository folders are required.
Install the development packages on Debian/Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config librtmidi-dev libserialport-dev libasound2-dev
```

On macOS, install `cmake`, `pkg-config`, `rtmidi`, and `libserialport`
with Homebrew. This folder includes `berry` for macOS ARM64 and
`berry.linux-arm64` for Linux ARM64 (including Rockchip). The build, R key,
and `series2patch.sh` automatically select the Linux ARM64 binary on that
platform and `berry` otherwise. Other platforms need a compatible Freshwater
Berry compiler placed here as `berry`. Keep the binaries executable.

```sh
cd voice_bd
make
./voicebd piano.wav
```

An optional second argument selects the firmware program:
`./voicebd piano.wav /path/to/channel.bec`. If omitted, `channel.bec` is
read from the current working directory.

`make` produces `voicebd` and `channel.bec` in this folder. Intermediate
build files go in `.build`. When copying the folder to another machine, omit
`.build` and the generated `voicebd` and `channel.bec` files so they are rebuilt.

The build runs the selected local Berry compiler with `channel.be -o channel.bec` using the included
`channel.be`. The compiled program includes
the firmware header and checksum required by the uploader. External compilers
must emit this same format.

Set the two port names in `voice_board_config_t` in `voicebd.h` before building.
Override them at runtime with `--usb-port PORT --rs485-port PORT`.
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

Startup reads `vq` before uploading programs. RS485 transactions retain their
existing 5 ms reply timeout and are not automatically retried. CDC HELLO/upload
requests have a 3-second reply deadline; stalled partial writes time out after
one second and require reopening the connection.

Input: 48 kHz, 16-bit PCM WAV, mono or stereo. Build requires RtMidi, libserialport, CMake and pkg-config; other dependencies come from the repository.

Use `-h` to display the version, product identifier, and usage.

The voice board API returns `voice_board_result_t` with an error code and message.
The test program checks these results and prints failures before exiting.

## Replacing a playing sample

`load_sample()` can replace a sample while its notes are playing. It uploads the
new attack directly into card memory while USB BODY streaming, status polling and MIDI
continue. Once the upload and root-pitch command succeed, the host swaps its PCM
under the stream mutex. Old allocations are freed outside that mutex.

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

One `voice_board_config_t::usb_port` selects the binary CDC port for all eight
voices, attacks and scripts. The old audio-device name and separate upload port
are removed. RS485 continues to carry controls and the existing 61-byte `vq`
reply. Linux/macOS use native CDC drivers and libserialport; RtAudio is not used.
Use this host with protocol-v1 firmware (`cafe:4032`); HELLO rejects incompatible
firmware. Re-enumeration can change the OS port suffix/path, so update the port
configuration for the board's unchanged `CHCARD-<UID>` identity.

The USB worker uses nonblocking I/O, a readiness wait and explicit MIDI/status
wakeups. Active voices retain five-ms demand forecasting with a two-ms reserve;
the worker may wake between status replies to refill them. There are no audio
callbacks, timed idle packets or `sp_drain()` calls per USB block. Each BODY block carries an
eight-byte header and up to 1024 signed-int8 source samples for one voice.
No blocks are sent when there is nothing to stream or upload. A released note
still needs data during its script-defined tail; `n off` hard-stops all voices.

On macOS the USB and status workers request interactive QoS and a preemptible
real-time policy (1 ms period, up to 250 microseconds of CPU per period). This
replaces the scheduling previously provided by the audio callback. The workers
still sleep when idle; scheduling policy does not send USB data. Linux uses the
host's normal thread policy and needs separate hardware timing qualification.
Note-off and all-notes-off remain available over RS485 after USB failure.

`vq` supplies exact writable credit. Prepared/unacknowledged samples, including
older sessions, remain charged against the physical voice ring. At most 8256
wire bytes are outstanding, with ready blocks batched into each USB write.
Prediction lasts at most five ms after fresh status; firmware retains a block
that does not yet fit, applying backpressure without dropping or overwriting it. Refills favor endangered voices, new-note priming
and voices with fewer samples in flight; upload chunks use spare capacity.
Polling is normally 5 ms and shortens to 1 ms for high card-reported per-voice
demand. A note-on ACK immediately wakes USB using existing credit. The card still
starts a note after its first 998 BODY samples, at an audio boundary.

Sample loading keeps the existing signed-16 host PCM and converts only the
transmitted attack/BODY bytes to signed eight-bit. Failed attack uploads leave
host PCM unchanged; partial attack writes on the card are not rolled back.
Script upload restrictions and validation remain owned by firmware.

Hardware acceptance must measure actual delivery gaps and note-on latency and
show zero `hold`, `full`, `drop`, `bad`, and late-refill increments at supported
loads. Host tests do not prove USB bus timing. See
[`CDC qualification`](../channel_card/docs/cdc_validation.md) and
[`wire protocol`](../docs/protocol.md).

Run the host/firmware-ring integration tests without a board:

```sh
cmake -S voice_bd -B /tmp/voicebd-tests
cmake --build /tmp/voicebd-tests --target voicebd_transport_test
ctest --test-dir /tmp/voicebd-tests --output-on-failure
```

## Mainframe synchronization

The mainframe is not migrated by this change. Its `voicebd.h` configuration and
build dependencies must be updated together with `voicebd.cpp` before using the
new firmware; the existing sync script copies only the implementation.

`voice_bd/voicebd.cpp` is the authoritative implementation. From the firmware
repository root, run `scripts/sync_voicebd.sh /path/to/175-mainframe` to copy it
into the existing `mas/voicebd.cpp`. The command shows the diff, skips identical
files, and saves a backup before replacement. It never changes `voicebd.h`
or commits to SVN. Synchronization does not run during `make`.

## USB-only hardware check

Close normal playback and run `./voicebd --usb-bench /dev/cu.usbmodemXXXXX 5`
(on Linux use the board's `/dev/ttyACM*` path). New protocol-v1 firmware is
required. No RS485 adapter or MIDI input is needed. The diagnostic verifies
byte/block counts and a rolling checksum, then reports sustained payload
throughput in eight- and 32-block batches and 64 host-to-firmware round trips. The test does not play notes;
its timing is not a note-to-DAC measurement.
