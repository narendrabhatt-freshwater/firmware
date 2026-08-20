# cardlink — Freshwater C++ host SDK

The single C++17 host library for Freshwater card control:

| Namespace | Role |
| --------- | ---- |
| `cardlink::SerialPort` | Shared byte pipe (termios / Win32) |
| `cardlink::rs485` | Tagged link, `Bus` bootstrap, and threaded `Controller` |
| `cardlink::usb` | CDC console transport + `al` attack-head upload (`AttackUploader`) |
| `cardlink::sample` | Plug-and-play SAMPLE session: attack upload, UAC body mix, notes |
| `cardlink::midi` | MIDI input, pitch helpers, and 8-voice FIFO allocation |
| `cardlink::audio` | Local speaker and Channel Card UAC output |
| `cardproto` | Command formatting, reply parsing, and typed card clients |

The archive includes the `cardproto` implementation. Existing build-tree
consumers may continue to link `cardproto::cardproto`, which aliases the same
target. The normative wire contract is
[`docs/protocol.md`](../docs/protocol.md).

## Build and consume

```bash
cmake -S protocol -B protocol/build -DBUILD_TESTING=ON
cmake --build protocol/build
ctest --test-dir protocol/build --output-on-failure
cmake --install protocol/build --prefix <prefix>
```

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

cardlink::sample::Client smp;
smp.SetCdcPath("/dev/cu.usbmodemCHCARD1");
smp.SetConsole([](const std::string &cmd) { /* send to Channel RS485 */ });
smp.LoadWave(0, "piano.wav", err);
smp.NoteOn(0, 261.63);
/* UAC callback: smp.Render(buf, nframes); */
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
bus.SetIdleHandler([](uint8_t slot) { /* stop this slot's UAC dry stream */ });
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
