# Release qualification

`cmi_core` may be tagged only after all software checks and the first-release
hardware records below are complete. Later releases repeat hardware checks
after USB, audio, serial, or protocol changes and periodically otherwise.

## Software gate

Run `scripts/qualify.sh`. CI must pass on macOS Intel and Apple Silicon,
Ubuntu x86_64, and Linux ARM64. Confirm the standalone SVN export contains the
CLI, examples, documentation, licenses, tests, and qualification script.

## Hardware record

For each certified host family, record date, OS image/version, architecture,
board firmware revisions, adapter model, Git SHA, sample sizes, stage timings,
throughput, playback duration, and xrun count. Exercise:

- `cmi-play doctor`, including a deliberate serial-permission failure;
- automatic and explicit selection, plus an ambiguous-adapter case;
- connect, voice query, disconnect, reconnect, and unplug recovery;
- all-eight-voice `channel.be` upload;
- user WAV and raw sample uploads plus user wavetable uploads;
- MIDI note/retrigger/steal/release and sustained BODY streaming.

Compare like-for-like timings with the accepted baseline. Investigate and
approve any regression greater than 20% before tagging.

## Release

- Confirm organization-owned or formally allocated production USB VID/PIDs
  have replaced the development `0xCAFE` identifiers in firmware and host
  discovery.
- Update `CHANGELOG.md`, remove the Unreleased marker, and set the CMake version.
- Merge to the release line and tag Git `cmi_core-v1.0.0`.
- Publish the matching SVN tag `v1.0.0` and archive qualification records.
