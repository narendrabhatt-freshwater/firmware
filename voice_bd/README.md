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

## Linux / Rockchip ARM64 setup

Use the included ARM64 Berry compiler and the Debian/Ubuntu dependencies above.
The two transport workers require permission for `SCHED_RR` priority 20. For a
PAM login session, an administrator can add these lines to
`/etc/security/limits.d/90-voicebd.conf`, replacing `YOUR_USER` with the account
running the application:

```text
YOUR_USER soft rtprio 20
YOUR_USER hard rtprio 20
```

Log out and back in, then check `ulimit -r` reports at least `20`. This requires
the login service to enable `pam_limits`. For a systemd service, use
`LimitRTPRIO=20` in its `[Service]` section instead. The application raises only
its USB and RS485 workers; do not run the whole application with `chrt` or as
root. Linux permissions and policy behavior are described in the
[Linux scheduler documentation](https://man7.org/linux/man-pages/man7/sched.7.html).

The RS485 adapter also needs prompt delivery of short replies. For an FTDI
adapter exposing `latency_timer`, configure 1 ms with a udev rule in
`/etc/udev/rules.d/90-voicebd-rs485.rules`:

```text
ACTION=="add", SUBSYSTEM=="usb-serial", DRIVER=="ftdi_sio", ATTR{latency_timer}="1"
```

Reload udev rules and reconnect the adapter. This rule applies to FTDI serial
adapters; other drivers may not expose this setting. The host accepts an
already configured timer without needing write permission, otherwise attempts
to set it and verifies read-back. An existing timer that cannot be set to 1 ms
causes a clear startup error. The Channel Card's USB CDC stream does not use
this FTDI setting.

Set `--usb-port` to the Channel Card's `/dev/ttyACM*` port and `--rs485-port` to
the adapter's `/dev/ttyUSB*` port, or use stable `/dev/serial/by-id/` paths.
The application account also needs serial-device access (usually the `dialout`
group on Debian). No RtAudio or additional scheduling library is required.

## Terminal sound browser

`make` also builds `voicebdgui`, a standalone terminal browser in
`voicebdgui.cpp`. It uses the existing CDC/RS485 transport and RtMidi; it adds
no UI or audio library and does not depend on or modify `175-mainframe/mas`.

```sh
./voicebdgui
# Optional overrides:
./voicebdgui --library /Users/narendrabhatt/cmi-local/soundlib \
  --usb-port /dev/cu.usbmodem13301 --rs485-port /dev/cu.usbserial-BG03CSYB \
  --midi-port 0
```

The default library is `~/cmi-local/soundlib`. Files are found recursively and
shown with their relative directories. The browser initializes the card with
`series2.bec` beside the executable and starts MIDI input (`MIDI_PORT` from
`devices.h`, currently 0 on macOS). `--list-midi` lists available inputs;
`--bec FILE` selects another program. The default attenuation is 18 dB;
`--attenuation 0..127` overrides it (0 is loudest).

| Key | Action |
| --- | --- |
| Up/Down, Page Up/Down, Home/End | Select a file |
| `/` | Enter a case-insensitive filename/directory search |
| Enter or Esc while searching | Finish editing the search |
| Backspace / Ctrl+U while searching | Delete a character / clear the search |
| `L` or Enter outside search | Load the selected file |
| Esc outside search | Clear the filter |
| Space outside search | Stop all notes |
| `Q`, Ctrl+C, or Ctrl+D outside search | Stop playback and exit |

After loading, play the MIDI keyboard. Eight voices, velocity, sustain pedal
(CC64), repeated notes, and oldest-voice stealing are supported. MIDI channel
ownership is tracked separately. Pitch bend and aftertouch are not implemented.
Loading stops current notes and discards MIDI received during conversion/upload;
play new notes after the status says **Loaded**. Files and screen drawing run
outside the transport workers. The MIDI worker polls every 1 ms independently
of terminal input. This is not a measured end-to-end latency guarantee.

Supported input:

- **Series I/II `.VC`**: 21,760-byte files, modes 1–6, revisions 0–4. Layout
  comes from `175-mainframe/S2XSRC/S2XP2SRC/LOADF.SA`; sample ordering and
  Hermite resampling match MAS `wrwave()` / `voice.cpp` / `resample.cpp`.
  The decoder reads the 16 KiB unsigned waveform, mode-4 loop descriptor,
  and revision-4 embedded STSEG setting, including old loop revision fixes.
  It assumes MAS's 14,080 Hz source rate. This is **waveform audition** with
  the `series2` envelope: external `.CO` files, embedded control/envelope
  programs, original tuning controls, and mode-1 synthesis are not emulated.
- **`.wav`**: RIFF signed 16-bit PCM, mono or stereo, 1–192 kHz. Stereo is
  mixed to mono; samples are converted to 48 kHz for the board.

Like MAS, loops are rendered ahead of playback into finite PCM. The default is
30 source seconds (`--loop-seconds 1..120`); held notes become silent when that
buffer ends. Higher notes reach the end sooner. Six octave sample banks keep
upper MIDI octaves from being folded down by the firmware's 2x rate limit.
Bank conversion uses the existing MAS interpolation, not a new antialiasing
filter. Unsupported or corrupt files display an error and leave notes disabled
until another sound loads successfully. Sound library files are only read.

Without hardware:

```sh
./voicebdgui --browse-only           # Browse, search and decode; no MIDI/serial
./voicebdgui --check /path/sound.VC   # Decode and report PCM length
./voicebdgui --check-library         # Validate every VC/WAV; nonzero if any fail
cmake --build .build --target voicebd_gui_test
ctest --test-dir .build --output-on-failure
```

Validation on the connected Mac/Channel Card: 1,524 library voices decoded;
`iixsoundlib/GUITARS1/FUZZGTR3.VC` was rejected for an invalid mode/revision
header. Decoder/resampler/MIDI unit tests and existing transport tests passed.
A short automated terminal/hardware run covered search, orchestra/piano loads,
an eight-note chord, sustain, an upper-octave note, replacement while held,
Space, malformed-file recovery, and quit while playing. Firmware drop/hold/
full/bad/late/future counters stayed zero; minimum ring fill was 1,512 samples.
This checks data delivery and controls, not acoustic fidelity or note-to-DAC
latency. The Minilab3 was detected as input 0; the automated notes used a
temporary CoreMIDI source.

## WAV command-line tester

An optional second argument selects the firmware program:
`./voicebd piano.wav /path/to/channel.bec`. If omitted, `series2.bec` is
read from the current working directory.

`make` produces `voicebd`, `voicebdgui`, `series2.bec`, and `channel.bec` in this folder. Intermediate
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

Startup sends one `clear` command before opening USB and uploading programs.
With matching firmware, `clear` is an alias for `n off`: it hard-stops all voices,
discards their queued playback data and resets Berry state before replying. No sleep, retry or additional status polling is added. Older firmware
does not implement `clear`, so update the Channel Card firmware for this recovery
behavior. USB open separately resets transport queues and partial messages.

Startup reads `vq` after uploading programs, establishing the processed BODY count before streaming. RS485 transactions retain their
existing 5 ms reply timeout and are not automatically retried. USB upload
requests have a 3-second reply deadline; stalled partial writes time out after
one second and require reopening the connection.

Input: 48 kHz, 16-bit PCM WAV, mono or stereo. Build requires RtMidi, libserialport, CMake and pkg-config; other dependencies come from the repository.

Use `-h` to display the version, product identifier, and usage.

While `voicebd` is running, type a colon command and press **Enter**:

| Command | Meaning |
| --- | --- |
| `:a 5000` | Five-second attack; durations are in **milliseconds** (`:a 5` is 5 ms). |
| `:s 1` | Sustain/peak amplitude, from 0 to 1. There is no separate decay stage. |
| `:r 250` | Release from the current amplitude to zero in 250 ms, including when released during attack. |
| `:g 0.01` | Linear gain from 0 to 1, applied by the firmware envelope after sample conversion. |
| `:e off` | Disable attack/release and play at `:g` immediately until note-off. |
| `:e on` | Enable attack/release again; the stored timings and sustain are retained. |
| `:status` / `:help` | Show the current settings or command help. |

Zero attack or release is immediate. With the envelope enabled, the note rises
to `sustain * gain`; with it disabled, the level is `gain` and sustain is ignored.
MIDI velocity does not change these durations or audio levels. The existing
voice-steal fade remains in place for replacements. The card's separate fixed
voice mixer gain and DAC attenuation still apply; `:g` is not the RS485 DAC `g`
command and does not change the speaker volume setting.

The startup `.bec` file remains active until the first setting command. That
command selects terminal envelope controls, initially **attack 5000 ms, sustain
1, release 5 ms, gain 1, envelope on**, with the requested value applied. Settings
last for the current run. Each change is compiled first, then stops existing
notes and uploads the program to all eight voices; **press a MIDI key again
after "Applied"**. A malformed command or failed compilation does not replace
the playing program. An incomplete upload pauses new note-ons until a complete
settings upload or `R` reload succeeds.

Generated `voicebd_live.be` and `voicebd_live.bec` are placed alongside the
executable, using the Berry compiler in the same folder. Original `.be`/`.bec`
files are not overwritten by colon commands. **Esc** cancels command entry;
Backspace edits it and Ctrl+U clears the text. MIDI continues while typing.
Outside command entry, the existing `S` sine-wave shortcut and other single-key
controls still work. `R` returns to the regular `series2.be` program.

For a fixed-level listening check, enter `:g 0.01`, then `:e off`, and play a
note. For the five-second fade, use `:e on`, `:a 5000`, `:s 1`, and `:g 1`.

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

Use the simulator and client from this branch together. RS485 carries
controls and the 56-byte type-0x0F `vq` reply. Each reply contains an active mask,
a 16-bit processed BODY sequence, and eight records: voice, playing session,
free samples and samples needed over 5 ms. Session 255 means no current stream.
The shared buffer capacity is fixed at 4080 samples per voice.

BODY (type 1) has a five-byte header: type, voice, session and little-endian
16-bit payload length, followed by signed 8-bit PCM. It never receives a USB
reply, including on error. Malformed BODY still faults reception.

UPLOAD (type 2) has one seven-byte header: type, kind, target and little-endian
32-bit total size. Exactly that many data bytes follow, with no chunk headers.
Kind 1 uploads an attack, 2 a wavetable and 3 a Berry program. The receiver
processes arriving data incrementally and replies once after the full upload.
Success is three bytes: target, upload command (2), status (0). An error uses
status 1 followed by text ending in LF. Invalid framing faults the connection;
a rejected upload with valid framing is consumed through its declared end.
There are no begin, offset, abort, HELLO or probe messages. BODY frames cannot
be inserted between an upload header and its last data byte.

The status sequence counts processed BODY frames, including obsolete sessions,
and wraps at 65536. It lets the client subtract samples still in transit from
reported free space. Status requests are serialized on RS485, so no separate
status sequence is needed. The client reads the initial sequence after program
uploads have completed on the freshly opened USB connection.

The client tracks the replacement notes it sends. The reported playing session
identifies when the script switches notes. Current and replacement notes share
the pool; acknowledged replacement data is subtracted when estimating the old
note's coverage. BODY chunks are aligned to the allocator's 16-sample blocks.
Coverage reserves the partially consumed block and interpolation samples. It
is an estimate; no remaining-time value is sent by the simulator.

The existing 5 ms status interval and 5 ms RS485 reply timeout remain unchanged.
USB/status worker scheduling remains unchanged. Linux and STM32 have not been
qualified with this experimental protocol.

Sample loading keeps the existing signed-16 host PCM and converts only the
transmitted attack/BODY bytes to signed eight-bit. Failed attack uploads leave
host PCM unchanged; partial attack writes on the card are not rolled back.
Script upload restrictions and validation remain owned by firmware.

Hardware acceptance must measure actual delivery gaps and note-on latency and
show zero `hold`, `full`, `drop`, `bad`, and late-refill increments at supported
loads. Host tests do not prove USB bus timing. See
the [Channel wire protocol](../channel_card/PROTOCOL.md).

Run the browser decoder, resampler and MIDI tests without a board:

```sh
cmake -S voice_bd -B /tmp/voicebd-tests
cmake --build /tmp/voicebd-tests --target voicebd_gui_test voicebd_stream_test voicebd_rs485_event_test
ctest --test-dir /tmp/voicebd-tests --output-on-failure
```

## Mainframe synchronization

The mainframe must use matching `voicebd.cpp` and `voicebd.h` files, the single
`usb_port` configuration, and serial device defaults appropriate for its OS.
Remove the obsolete RtAudio dependency from its makefile. MAS still uses
PortAudio separately for its existing software audio path.

MAS also needs `series2.bec` at startup and `series2.be`, `series2patch.sh` and
the compatible `berry.linux-arm64` compiler for runtime patch changes. The
ARM64 CMI builder now exports these alongside `mas.linux`. Preserve MAS's own
`series2.be` settings when compiling that program.

`voice_bd/voicebd.cpp` is the authoritative implementation. From the firmware
repository root, run `scripts/sync_voicebd.sh /path/to/175-mainframe` to copy it
into the existing `mas/voicebd.cpp`. The command shows the diff, skips identical
files, and saves a backup before replacement. It never changes `voicebd.h`
or commits to SVN. Synchronization does not run during `make`.
This helper alone is insufficient for migrating an older UAC checkout: update
the header, device configuration and build dependencies as described above.
