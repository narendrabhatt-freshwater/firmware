# voicebdtest

```sh
cd voice_bd
make
./voicebdtest piano.wav
```

An optional second argument selects the BEC program:
`./voicebdtest piano.wav /path/to/channel.bec`. If omitted, `channel.bec` is
read from the current working directory.

`make` produces `voicebdtest` and `channel.bec` in this folder. Intermediate
build files go in `.build`.

Set the three port names in `voice_board_config_t` in `voicebd.h` before building.
The program uses those names and MIDI input 0, loads the sample for all eight
voices, and asserts on board failures. Ctrl+C stops playback and exits.

Input: 48 kHz, 16-bit PCM WAV, mono or stereo. Build requires RtAudio 6,
libserialport, CMake and pkg-config; other dependencies come from the repository.
