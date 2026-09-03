# CMI Play CLI

`cmi_play` runs the complete performance setup without the GUI. The `fw play`
wrapper builds it, discovers the hardware, uploads assets, opens MIDI, and
manages the BODY stream as notes play.

Create a project folder like this:

```text
my-sound/
├── channel.be
├── samples/
│   ├── w60_piano_head.i8
│   └── w60_piano_body.i8
└── wavetables/
    ├── osc0.wav
    └── osc1.raw
```

Then run:

```sh
fw cli

# Or use your own project:
fw play my-sound --rs485 /dev/cu.usbserial-0001
```

`fw cli` uses the repository's existing Channel example and loads one sample
mapped across all 128 MIDI keys, avoiding the old 704 MB/248-sample startup.
The RS485 adapter, Channel CDC, and MIDI input are selected automatically.
While it runs, saving `channel.be` compiles and uploads it to all eight voices.
Press Ctrl-C or enter `quit` to silence the card and exit.

The interactive `cmi>` prompt accepts `status`, `reload [FILE]`, `samples DIR`,
`wavetable N FILE`, `root ID HZ`, `gain DB`, `filter VOICE HZ [Q]`, `off`,
`help`, and `quit`.

Use `--cdc`, `--midi`, or `--audio` only when automatic selection is ambiguous.
Use `fw play --help` for all options.

The sample folder also accepts combined `wN_*.wav`, `wN_*.raw`, or `wN_*.i8`
files. A `roots.txt` file is applied automatically when present.

Every startup provisions all eight oscillator slots. Files named
`wavetables/osc0...osc7` override individual slots; missing slots receive the
built-in sine, triangle, saw, square, pulse, organ, bright, and noise tables.
