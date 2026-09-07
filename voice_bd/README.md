# voicebdtest

```sh
cd voice_bd
make
./voicebdtest piano.wav
```

An optional second argument selects the firmware program:
`./voicebdtest piano.wav /path/to/channel.fwsc`. If omitted, `channel.fwsc` is
read from the current working directory.

`make` produces `voicebdtest` and `channel.fwsc` in this folder. Intermediate
build files go in `.build`.

To use the standalone compiler in `175-mainframe/mas`, configure from this folder:

```sh
cmake -S . -B .build -DBERRY_EXECUTABLE="$PWD/../../175-mainframe/mas/berry"
make
```

The build runs `berry channel.be -o channel.fwsc`. Raw `.bec` output is not
accepted by this uploader.

Set the three port names in `voice_board_config_t` in `voicebd.h` before building.
The program uses those names and MIDI input 0, loads the sample for all eight
voices, and reports board errors before exiting with a nonzero status. Ctrl+C stops playback and exits.

Startup assumes the card is idle. If voices are active, program upload fails with
`err:vm-busy`; startup does not silence them or wait for their release.

Input: 48 kHz, 16-bit PCM WAV, mono or stereo. Build requires RtAudio 5.2 or 6,
libserialport, CMake and pkg-config; other dependencies come from the repository.

The command-line parser uses the shared Freshwater `options.h` header, as in
`mas/extract.cpp`. Supply its directory in the compiler include path (`-I`).
Use `-h` to display the version, product identifier, and usage.

The voice board API returns `voice_board_result_t` with an error code and message.
The test program checks these results and prints failures before exiting.
