# Channel Card simulator

Runs the Channel Card application on a Mac or Linux host so that protocol,
console, script, and playback logic can be checked without a card. The
application sources in `app/` and `berry_runtime/` are compiled unchanged.
Only the four hardware files are replaced:

| Firmware file | Simulator replacement |
| --- | --- |
| `app/board_stm32.cpp` (UART5, RS485 pins, LEDs, switches, tick) | `sim/board_sim.cpp` |
| `app/usb_device.cpp` (USB CDC device) | `sim/board_sim.cpp` |
| `app/audio_i2s.cpp` (I2S, DMA, TIM7) | `sim/board_sim.cpp` |
| `app/cs4304.cpp` (DAC over I2C) | trim values recorded, no DAC |

This folder is a development tool and is not part of the SVN package.

## Build

From `channel_card/app/`:

```sh
make simulator
```

or from `channel_card/sim/`: `make`. It needs a host C and C++ compiler and
GNU Make; the Arm toolchain is not used. The program is `channel_card/bin/sim`,
beside `channel_MCU.bin`. Intermediate files go to `channel_card/build/sim/`.

| Target | Result |
| --- | --- |
| `make` | `bin/sim` and the test program |
| `make test` | Run the scripted tests |
| `make apps` | `voice_bd/voicebd.sim` and `voice_bd/voicebdgui.sim` (needs RtMidi) |

## Running

`sim` is only the card. It listens on two Unix sockets, `/tmp/channel-usb`
for the USB CDC port and `/tmp/channel-rs485` for the RS485 line, and waits
for a host:

```sh
channel_card/bin/sim -ns
```

Options are single letters after one dash and can be combined, as in the
other tools (`voice_bd/options.h`). A letter followed by `=` takes a value.

| Option | Meaning |
| --- | --- |
| `-n` | Log each voice starting, being replaced, and ending, read from card state |
| `-s` | Log one activity line per second |
| `-w=FILE` | Record DAC channel 1 as 16-bit mono WAV |
| `-o=FILE` | Stream channel 1 as raw signed 16-bit PCM; `-o=-` is stdout |
| `-t=S` | Exit after S seconds of card time |
| `-c=PATH`, `-l=PATH` | Sockets for the USB CDC port and the RS485 line |
| `-h` | Help |

Logs go to stderr, each line starting with card time in seconds. Errors are
always logged: link faults, BODY underruns and drops, RS485 receive overflow,
and script faults. So is a host opening or closing a port.

```text
    0.754  usb host opened the port
    1.204  voice 0 start: key 60 velocity 100 session 1 sample 0
    2.000  awake  1.9%  wakeups  1049  audio 1000  usb  770  rs485  211  main   979  late  0.43ms  hold 0  drop 0
```

The `-s` line reports how much of the second the simulator was awake,
wake-ups in total and by source, the worst late audio request, and the
card's `hold` and `drop` counters. The awake figure is host time and says
nothing about the Cortex-M7 load.

### Host programs

The host is the production host code, run as a separate program. The `.sim`
builds from `make apps` take the socket paths as their ports (they need a
MIDI input):

```sh
cd voice_bd
./voicebd.sim piano.wav --usb-port /tmp/channel-usb --rs485-port /tmp/channel-rs485
./voicebdgui.sim --usb-port /tmp/channel-usb --rs485-port /tmp/channel-rs485
```

`voicebd.cpp` is compiled unchanged. The `.sim` builds link
`sim/host/serialport.cpp` in place of libserialport, because the real library
cannot open a pseudo-terminal on macOS. Opening the USB socket stands in for
raising DTR; closing it stands in for unplugging.

### Listening

`sim` has no audio output of its own. Record with `-w=FILE`, or pipe the raw
stream into a player:

```sh
channel_card/bin/sim -o=- | play -q -t raw -r 48000 -e signed -b 16 -c 1 -   # SoX
channel_card/bin/sim -o=- | aplay -q -f S16_LE -r 48000 -c 1                 # Linux
```

## How it models the card

The firmware has three interrupt sources and a main loop. The simulator has
the same three event sources and runs the same main-loop pass
(`ChannelApp_Step()`), on one thread:

1. USB bytes arrive from the host.
2. RS485 bytes arrive from the host.
3. The audio clock asks for frames.

After each event it runs main-loop passes until one does nothing, then waits
for the next event. That wait is where the processor would sleep.

- **Audio.** The DAC reads from the same two circular DMA buffers as
  `audio_i2s.cpp`: 96 frames each, I2S1 for channels 1 and 2, I2S2 for 3 and
  4, prefilled the same way at start. Each time the DAC leaves half a buffer,
  the half/complete interrupt refills that half with 48 frames, so a refill
  is heard 1 ms after it is rendered and script handlers run once per refill.
  `sim` asks for one refill at a time. The DAC may take any number of frames
  per request without changing the audio; `make test` checks 1, 7, 64, 480,
  and 4096.
- **Time.** Card time is the audio clock: one millisecond per refill. In
  `sim` the audio clock follows the host's clock. In `sim_test` it advances
  only when a test asks for audio, so every run is identical.
- **USB.** Same 8192-byte receive queue and 2048-byte transmit queue as
  `usb_device.cpp`. The parser gets at most 64 bytes per read. A full queue
  stops reading from the host, which is the OUT endpoint NAKing.
- **RS485.** Same 8192-byte receive ring; a full ring drops bytes and counts
  them. Replies are sent at once.

## Tests

`make test` runs each test in a fresh process on a freshly booted card. The
test plays the host over the same two byte streams: HELLO, uploads, note
commands, `vq` polling, and BODY blocks sent against reported credit. Covered:
boot defaults, console replies and addressing, HELLO and its failure cases,
malformed and stalled frames, script and attack uploads, one note, eight
voices and `clear`, a steal, USB backpressure, a starved host, a late audio
clock, RS485 receive overflow, and request-size invariance.

## What it does not show

- Whether the mix fits the 1 ms refill on the Cortex-M7. Host timing is not
  comparable.
- Interrupt preemption. An event never interrupts a main-loop pass here, so
  races that depend on that are not exercised.
- The real USB device layer: enumeration, control requests, endpoint state,
  zero-length packets.
- RS485 electrical behaviour: turnaround, bus contention, baud error, wire
  time. The UART5 interrupt handler itself is not run.
- I2S, DMA, the I2S2 underrun guard, the DAC, and anything analog.
- Bit-exact audio. Host floating point can differ slightly from the M7.

Hardware timing and audio acceptance still need the card.
