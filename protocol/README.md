# cardlink — Freshwater C++ host SDK

The single C++17 host library for Freshwater card control:

| Namespace | Role |
| --------- | ---- |
| `cardlink::SerialPort` | Shared byte pipe (termios / Win32) |
| `cardlink::rs485` | Tagged link, `Bus` bootstrap, and threaded `Controller` |
| `cardlink::usb` | CDC console + `al` attack upload + packed UAC2 BODY framing |
| `cardlink::sample` | Plug-and-play SAMPLE session: attack upload, BODY mixer, notes |
| `cardlink::midi` | MIDI input, pitch helpers, and 8-voice FIFO allocation |
| `cardlink::audio` | Local speaker and Channel Card BODY stream (`SampleBulkOut`, UAC2 int16) |
| `cardproto` | Command formatting, reply parsing, and typed card clients |

The archive includes the `cardproto` implementation. Existing build-tree
consumers may continue to link `cardproto::cardproto`, which aliases the same
target. The normative wire contract is
[`docs/protocol.md`](../docs/protocol.md).

## Editable crash sequence

The `protocol` executable can play a hand-edited timing sequence through one
Channel Card voice:

```bash
protocol/build/protocol --crash protocol/examples/crash.txt
```

Compact options mirror the protocol-oriented workflow: `--g 6` sets output
attenuation (the Channel Card gain command is `g`), `--r 1` runs one pass,
`--s FILE` uses one sample file, and `--d DIR` selects a sample bank.

The runner automatically finds the Channel Card CDC and the working RS485
adapter, locates the repository's `cmi_control/waves` bank, uploads the attack
heads required by the sequence, loads their BODY samples and roots, and opens
the Channel Card UAC BODY stream. `C4` selects wave `w60_*`, `D4` selects
`w62_*`, and so on. No sample path is required. To use one file for every note,
pass `--sample sound.wav`; `--samples DIR` remains available for a complete
`wN_*.raw` bank.

Check parsing and note-to-wave mapping without opening hardware with
`--dry-run`. `--repeat 1` runs one pass regardless of the file's `repeat`
instruction. Ctrl-C always sends all notes off before closing the bus.

```text
c4 200 2
en 1.0 100 0.7 10 20
d4 200
en 1.0 50 0.5 5 10
w 500
repeat
```

`c4 200 2` plays C4 for 200 ms, configures the card's real 2 ms voice-steal
ramp, and leaves C4 active for the following note to replace. Omitting the last
value sends note-off after the hold. `w 500` is the only synthetic delay: it
waits 500 ms.
An `en ...` line belongs to the note directly above it; the runner applies it
to that note's voice immediately before note-on. It accepts the firmware's full
envelope grammar, including up to 10 segments. Other normal Channel Card
commands pass through unchanged. A bare `repeat` loops until Ctrl-C;
`repeat N` runs exactly N passes.

## Build and consume

```bash
cmake -S protocol -B protocol/build -DBUILD_TESTING=ON
cmake --build protocol/build
ctest --test-dir protocol/build --output-on-failure
cmake --install protocol/build --prefix <prefix>
```

After flashing the Channel Card, the hardware smoke test opens and closes the
10-channel 51 kHz signed-int16 UAC carrier once before reopening it for the
measurement. This catches the historical second-open failure as well as BODY
stream errors:

```bash
protocol/build/cardlink_sample_hw_smoke c6x3c5 12 <body.raw> <rs485-port> <cdc-port>
```

Voice-limit, stealing, and mixed-chord cases use the same arguments:

```bash
protocol/build/cardlink_sample_hw_smoke mono_steal 12 <body.raw> <rs485-port> <cdc-port>
protocol/build/cardlink_sample_hw_smoke duo_steal 12 <body.raw> <rs485-port> <cdc-port>
protocol/build/cardlink_sample_hw_smoke c6c4a4 12 <body.raw> <rs485-port> <cdc-port>
```

`mono_steal` plays C6 → C4 → A4 through physical voice 0. `duo_steal`
builds C6+C4, then adds A4 and verifies the FIFO steal path on voice 0.

It prints the requested source-sample rate and the exact framed UAC capacity;
an over-budget case is labelled before playback. The raw carrier is 510
samples/ms; direct BODY capacity is 509 samples/ms for any voice mix. The
smoke executable validates startup and steady-state
counters separately and exits unsuccessfully if either records a hold, drop,
bad/late packet, or host callback xrun.

Build-tree consumers use:

```cmake
add_subdirectory(path/to/protocol)
target_link_libraries(your_app PRIVATE cardlink::cardlink)
```

Installed consumers use:

```cmake
find_package(cardlink REQUIRED)
target_link_libraries(your_app PRIVATE cardlink::cardlink)
```

```cpp
#include "cardlink/sample/client.hpp"
#include "cardlink/audio/sample_bulk.hpp"

cardlink::sample::Client smp;
cardlink::audio::SampleBulkOut body;
smp.SetCdcPath("/dev/cu.usbmodemCHCARD1");
smp.SetConsole([](const std::string &cmd) { /* send to Channel RS485 */ });
smp.LoadWave(0, "piano.wav", err);
body.BindMixer(smp.Mixer());
body.Start(err);
smp.NoteOn(0, 261.63);
```

```cpp
#include "cardlink/rs485/bus.hpp"
#include "cardlink/usb/attack_upload.hpp"

cardlink::rs485::Bus bus;
cardlink::usb::AttackUploader up(port);
```

For applications that need asynchronous connection, note scheduling, typed and
raw command queues, recovery, and voice-status polling, use `Controller`:

```cpp
#include "cardlink/rs485/controller.hpp"

cardlink::rs485::Controller bus;
cardlink::midi::VoiceBank voices;
bus.SetLogHandler([](const std::string &line) { /* display or persist */ });
bus.SetIdleHandler([](uint8_t slot) { /* stop this slot's BODY stream */ });
bus.RequestOpen("/dev/cu.usbserial-0001", 921600, 6);
bus.PublishBank(voices);
bus.QueueChannel([](cardproto::ChannelClient &ch) {
  return ch.SetGain(1, 12);
});
```

`cmi_control` is the supported consumer example. Its `BusController` is a thin
UI adapter that sends controller logs to `LogBuffer` and converts successful
raw-command notifications into UI mirror patches.

Public headers are installed under `cardlink/` and `cardproto/`. RtMidi and
RtAudio are transitive dependencies of `cardlink::cardlink`.
