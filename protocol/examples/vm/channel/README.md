# Channel VM examples

These are editable policies, not firmware defaults. Each physical Channel Card
voice owns a separate program slot, so upload a program explicitly to voice
0..7 after every reset. Different files may be compiled for different voices;
uploading `current_envelope.be` to all voices simply gives them matching
behavior.

Initial hardware-test programs:

- `simple_envelope.be` — immediate note start, attack, decay, sustain, and
  release, with no crash fade.
- `current_envelope.be` — the normal attack/hold/release policy.
- `gate.be` — immediate full-level gate with a short release.
- `pluck.be` — fast attack followed by an automatic decay.

CMI Control presents these programs on the Tone page. It recompiles a
source when its cached FWSC is absent or older, silences the card, then uploads
the same container to voices 0 through 7 over the selected Channel CDC port.

The command-line equivalent accepts the `.be` file directly:

```sh
protocol/build/protocol --script protocol/examples/vm/channel/gate.be
```

Add `--dry-run` to compile and validate without opening the card, or `--u CDC`
to select a specific Channel Card port. The command always uploads the selected
program to all eight voices.

Each FWSC Berry payload is limited to 4 KiB. Upload is idle-only and activation
is atomic for the selected program object. All programs share one VM; unsafe
allocation, GC, or watchdog faults therefore invalidate all eight programs.
