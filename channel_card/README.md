# Channel Card firmware and commands

The STM32H725 Channel Card mixes eight scripted sample voices and optional
wavetable oscillators onto CS4304 DAC channel 1. Channels 2–4 provide control
voltages. RS485 carries commands and status; USB CDC carries framed binary
sample data and uploads.

The [Berry guide](../berry_compiler/README.md) covers the matching compiler,
script language, runtime functions, and examples. In an SVN release the
compiler is included in `berry_compiler/`. The [wire protocol](PROTOCOL.md)
specifies framing, uploads, and flow control.

## Build

Install GNU Make, a POSIX shell with standard tools (`awk`, `cmp`, `shasum`),
and the GNU Arm
embedded toolchain with newlib (`arm-none-eabi-gcc`, `arm-none-eabi-g++`, `arm-none-eabi-objcopy`,
`arm-none-eabi-ar`, and `arm-none-eabi-objdump` on PATH). No build dependencies
are downloaded. Python is not required. For systems with `sha256sum`, use
`make SHA256=sha256sum`.

From `channel_card/app/`, run:

```sh
make
```

This builds Release firmware and writes **`bin/channel_MCU.bin`**, beside `app/`.
The standalone `app/Makefile` compiles and links directly, keeping intermediate
files under `build/release/`. CMake and Ninja are not required.
Use `make -j4` to build with up to four parallel jobs.
Use `make clean` to remove this build and its output binary.

Every build prints the firmware size, built baud rate, toolchain, report timestamp,
checksum, and Flash/RAM allocation by memory region and section. To view the
last successful build without compiling or changing settings:

```sh
cat ../build/release/channel_MCU.info
```

The Makefile handles validation and reporting directly. The firmware package
contains no Python tools, shell-script files, or test harnesses.

The report includes used/free bytes and percentages for Flash, DTCM, AXI SRAM
(`RAM_D1`), `RAM_D2`, `RAM_D3`, and ITCM. Section sizes show sample storage,
the Berry arena, DMA buffers, and the heap/stack reservation. These are linker
allocations, including alignment, rather than measured runtime memory peaks.
The saved report stays under `build/release/`; `bin/` contains the firmware binary
(and `sim`, the host simulator, if `make sim` has been run).

The RS485 rate is set by `BAUDRATE ?= 921600` at the top of
[`app/Makefile`](app/Makefile). Edit that value for the normal build rate,
or override it for a build:

```sh
make BAUDRATE=3000000
```

Changing the rate rebuilds the affected code automatically. Plain `make`
returns to the Makefile's value. Configure the host to use the same rate,
with 8 data bits, no parity, and 1 stop bit (8N1).

## Flash

Install [STM32CubeProgrammer](https://www.st.com/en/development-tools/stm32cubeprog.html)
for your host OS; CubeIDE is not required.

1. Run `make` in `app/` to build the firmware.
2. Connect the USB data cable, move the BOOT switch up, and press reset.
3. In STM32CubeProgrammer, select USB, refresh the ports, and connect to the
   Channel Card bootloader.
4. Download `bin/channel_MCU.bin` at `0x08000000` with verification enabled.
5. After successful verification, move BOOT down and press reset to run.

Flashing uses the built binary; configure the desired baud rate before building.
With multiple bootloaders attached, select the Channel Card's USB port.

The running device uses USB VID/PID `cafe:4032` and a serial number derived
from its hardware UID: `CHCARD-<24 hex digits>`. Select the corresponding CDC
port on the host; its OS-assigned path suffix may change.

## Prepare playback

Programs, sample attacks, and wavetables are held in RAM and are lost on reset.
Compile a program with the bundled Berry compiler, negotiate binary USB HELLO,
and upload it to each voice that will be used. Upload the sample attack and
set its root frequency with `ar`. Then select the sample and start a note:

```text
c:ar 0 261.625565
c:n0 on 0 60 127 @1
c:n0 off
c:clear
```

End each command with carriage return and wait for its reply. Stream BODY data
for the selected voice/session according to `vq` credit, including during a
scripted release. The first pending note requires 998 BODY samples before it is
ready for its script handler; an oscillator declaration does not bypass this
gate. A voice without a loaded program returns `err:no-program`.

The script controls pitch, amplitude, and note activation/release. It can add
oscillators using eight uploaded wavetables. DAC channel 1 starts at 0 dB trim
with the analog bypass route enabled. There is no USB volume control.

## Console command reference

This table matches the parser in
`app/channel.cpp`. Commands are case-insensitive because
console input is converted to lowercase. End a command with carriage return.

On shared RS485, prefix commands with `c:`. Replies are tagged `[C]`.
The Channel USB port accepts only framed binary blocks.
Successful setters normally return `ok`. Common failures are `err:syntax`,
`err:range`, `err:unknown`, `err:no-program`, `err:busy`, `err:usb`, and `err:vm-busy`.

### Playback and card control

| Command | Action |
| ------- | ------ |
| `h` / `help` / `?` | Return the live command list. |
| `n0`…`n7 on <key> [velocity]` | Start voice 0…7 using MIDI key 0…127 and velocity 1…127; velocity defaults to 127. A valid script must already be loaded for that voice. |
| `n0`…`n7 on <key> <velocity> @<session>` | Start a streamed note and bind BODY session 0…254 before acknowledging. Key-only commands default to velocity 127. |
| `n0`…`n7 on <sample> <key> <velocity> @<session>` | Select sample 0…247 and arm a streamed note in one command. |
| `n0`…`n7 off` | Release one voice. |
| `clear` / `n off` | Hard-stop all eight voices and reset playback buffers and Berry state. |
| `g <channel> <dB>` | Set CS4304 attenuation: channel 1…4, attenuation 0…127 dB. |

### Filters

| Command | Action |
| ------- | ------ |
| `f` | Query effective cutoff, q, and pitch tracking for voices 0…7. |
| `f <Hz> [q]` | Set every voice's base cutoff. Range is 20…20000 Hz; `0` and `20000` bypass. Optional q is 0.5…10. |
| `f0`…`f7` | Query one voice's filter. |
| `f0`…`f7 <Hz> [q]` | Set one voice's base cutoff and optionally q. |
| `fk` | Query pitch tracking for every voice. |
| `fk <k>` | Set pitch tracking for every voice; k range 0…10. |
| `fk0`…`fk7` | Query one voice's pitch tracking. |
| `fk0`…`fk7 <k>` | Set one voice's pitch tracking. |

Pitch tracking uses `fc = fbase × (noteHz / 261.625565)^k`.

### Samples, scripts, and streaming

| Command | Transport | Action |
| ------- | --------- | ------ |
| Upload kind 1 | Binary USB | Upload sample attack ID 0…247 using 1…512 signed-int8 bytes. |
| Upload kind 2 | Binary USB | Upload logical oscillator wave 0…7 using 2…512 signed-int8 bytes. Firmware owns its physical bank placement. |
| `ar <id> <Hz>` | RS485 | Set the positive root frequency for attack ID 0…255. |
| `a` | RS485 | Query loaded attack count and the 256-bit loaded mask. |
| Upload kind 3 | Binary USB | Begin an FWSC ABI2 program upload to voice 0…7. Total container size is 20…16404 bytes. Send offset-checked chunks; the final reply confirms validation/commit. |
| `vm` | RS485 | Query the active-program voice mask. |
| `vm <voice>` | RS485 | Query active state, target, ABI version, and fault for voice 0…7. |
| `vm mem` | RS485 | Return shared VM arena and per-voice fault/cycle diagnostics. |
| `vq` | RS485 | Query active/pending masks, BODY sessions, target fill, and exact writable credit. RS485 uses the fixed 61-byte `vq` response. |
| `reset` | RS485 | Clear RS485 hardware RX FIFO, queued RX bytes, receive error flags, and partial command line; replies `ok:reset`. Does not reboot or clear audio/voice state. |
| `usb` | RS485 | Query BODY transport and underrun counters. |
| `rs485` | RS485 | Report receive drops, transmit failures, and reply truncations. |
| `usbdev` | RS485 | Report USB peripheral and endpoint state. |
| `usb 0` | RS485 | Clear BODY transport counters and return the new values. |

Send `c:reset\r` and wait for `ok:reset` before sending another RS485 command:
bytes already queued behind reset are discarded. The command must reach a
working console; it cannot reset the host adapter or recover a disconnected bus.
Lifetime RX-drop counters are preserved.

The former ASCII `al`, `wl`, and `vmload` USB operations are replaced by
binary upload kinds 1, 2, and 3. BODY blocks can be interleaved with upload chunks.
Full upload sequencing and reply fields are documented in
[wire protocol](PROTOCOL.md).

### Service diagnostics

`vm mem`, `usb`, `usbdev`, and `rs485` report runtime or transport state.

Pitch-track smoke with a loaded sample: `f0 300`, `fk0 1`, `n0 on 60` then
`n0 on 72` — corner should roughly double with the octave (query `f0`).

## Source layout and maintenance

- `app/`: shared application, replaceable device backends, and build configuration.
- `berry_runtime/`: first-party script runtime and shared ABI declarations;
  `third_party/berry/` retains the vendored interpreter.
- `core/` and `drivers/`: STM32 initialization, interrupt glue, HAL, and CMSIS.
- `channel_MCU.ioc`, startup assembly, and `STM32H725xG_flash.ld`: hardware and
  memory configuration.

The application is split by responsibility:

| Implementation | Contents | Header |
| --- | --- | --- |
| `audio.cpp` | Mono audio rendering and sample-clocked script intervals | `audio.h` |
| `audio_dac.cpp` | Calibrated control voltages and DAC slot packing | `audio_output.h` |
| `transport.cpp` | Bounded RS485 completion wait with USB service | `transport.h` |
| `audio_i2s.cpp` | I2S/DMA buffers and callbacks, I2S2 underrun guard | `audio_output.h` |
| `board_stm32.cpp` | UART5, RS485 pins, board outputs, clock, synchronization and profiling | `board.h`, `transport.h`, `board_stm32.h` |
| `cs4304.cpp` | CS4304 DAC register access, initialization, gain and mute | `cs4304.h` |
| `channel.cpp` | RS485 commands, LED patterns, script uploads | `channel.h` |
| `voice.cpp` | Note playback, envelopes, Berry adapter | `voice.h` |
| `filter.cpp` | Four-pole kernel and per-voice filter control | `filter.h` |
| `oscillator.cpp` | Wavetable oscillators, routing, interpolation helpers | `oscillator.h` |
| `samples.cpp` | Sample attack storage and playheads | `samples.h` |
| `stream.cpp` | Per-voice BODY rings | `stream.h` |
| `usb.cpp` | Binary protocol, sample uploads | `usb.h` |
| `usb_device.cpp` | CDC device on HAL PCD, descriptors, transfer queues | `transport.h`, `usb_device.h` |

The source list in `app/sources.mk` is shared by the firmware and simulator.
Only `audio_i2s.cpp`, `board_stm32.cpp`, `usb_device.cpp`, and `cs4304.cpp`
access STM32 registers or HAL. Shared sources use `board.h` and `transport.h`;
the host build needs no STM32 headers. `platform_memory.h` selects target memory
placement through the firmware build's `CHANNEL_PLATFORM_STM32` define.

`render_audio_samples(buffer, sample_count)` fills contiguous mono signed
32-bit samples at 48 kHz. The caller owns valid storage and calls from one
execution context. Zero samples does nothing. Scripts retain their existing
48-sample processing intervals across calls, so one 96-sample request and two
48-sample requests have identical results with the same inputs available at
the same sample positions. The renderer allocates no memory and does not render
ahead. Voice clear and transport reset do not reset its sample clock.

The output backend owns DAC packing and control-voltage calibration/slew.
`AUDIO_I2S_HALF_FRAMES` in `audio_i2s.cpp` selects DMA size (default 48), without
changing the application interval. Larger output buffers can change real-time
command latency and require hardware timing measurements. They do not change
the supported sample rate.

`transport.h` defines replaceable RS485 and USB byte interfaces. RS485 start
borrows the caller's buffer until completion or abort; completion means the
backend finished transmission, not that a receiver acknowledged it. Shared
`transport.cpp` preserves the bounded wait and USB servicing while UART
interrupts transmit. The device driver does not dispatch application protocols.
USB reads may return any prefix; writes copy the entire buffer or reject it
without accepting a prefix. A successful write means queued. Protocol framing,
uploads, session credits and pending replies remain shared application state.

USB connection generations invalidate old transport bytes and protocol state.
STM32 reset, configuration changes, DTR transitions and disconnect use the
existing reset paths; suspend/resume change readiness without starting a new
generation. Faulted receive remains blocked until an existing lifecycle reset,
while queued replies may drain. The simulator maps PTY connection/flush events
to generations; it does not emulate USB endpoints or electrical timing.

Each platform owns its main loop. Firmware calls `process_usb_messages()`,
`process_rs485_commands()`, `update_channel_leds()`, then `process_usb_messages()`
again to service USB after a console reply. The simulator calls the same USB
and RS485 functions and renders audio on its own schedule; it does not call
the LED update. Both use the same application and protocol implementations.
The simulator's no-op interrupt/barrier implementation is valid only under
its single-threaded contract. `CHANNEL_ENABLE_CYCLE_PROFILING` enables cycle
measurement in the firmware build. The simulator omits that instrumentation
and has no cycle-counter backend; its cycle diagnostic fields remain zero.

LED patterns and script colour/brightness requests remain application logic.
`board.h` exposes logical status outputs and RGB requests; `board_stm32.cpp`
owns GPIO mapping, RGB level quantization and software PWM. The main-loop pass
services that backend at the existing point between RS485 commands and status
pattern updates, preserving the PWM phase and tick rules. The simulator ignores
LED, switch and DAC-trim requests; it does not retain or emulate those outputs.

Use simple C++17 with plain functions and structs. Exceptions, RTTI, and
thread-safe local-static initialization are disabled. Do not add virtual
functions, STL containers, or dynamic initialization to this application.
Public interfaces use C linkage for generated C and the Berry runtime;
interrupt handlers must also retain C linkage. The interpreter and generated
STM32 sources keep their existing C implementation.

Keep shared sources in `app/sources.mk`; backend sources, compiler flags and
link settings belong in `app/Makefile` and `sim/Makefile`.
Preserve `USER CODE` markers when editing CubeMX-generated sources. All maintained directories use lowercase names, including HAL/CMSIS
subfolders. If CubeMX regeneration restores names such as `Core`, `Drivers`,
`Inc`, or `Src`, lowercase those folders and check the paths in `app/Makefile`
before building. Generated CMake files are not used by Channel Card. Channel USB
uses HAL PCD and the custom CDC device; do not enable a second USB middleware stack. After regeneration, check USB
initialization, interrupt routing, main-loop processing, custom sources, and
linker configuration before rebuilding.

Use four-space indentation, function braces on separate lines, and control-flow
braces on the opening line. Use 79-column section comments as in the MAS
`voicebd.cpp`, with a blank line before the following function or declaration:

```c
/* ---- read and write little-endian values -------------------------------- */

```

A section can group related small helpers. Start every application and simulator
source, header, test, and build file with the standard Freshwater ASCII/copyright
header from `sim/main.cpp`. Use `#` comments for headers and section dividers in
scripts and Makefiles, keeping script shebangs first. Comments explain contracts
and hardware constraints that the code cannot express. Preserve Freshwater file
headers, copyright notices, and vendor formatting. Local tests and development
tools are maintained separately from the SVN package.

Function names follow MAS: lowercase words separated by underscores, describing
the operation and its object. Use `process_rs485_commands()`, `render_audio_samples()`,
and `read_rs485_byte()` rather than implementation-specific names in shared code.
Use `read` for consuming input, `peek` for inspecting buffered input, `get` for
state queries, and `render` or `mix` for producing samples. Predicates use `is`,
`has`, or `can`; include units such as `_ms`, `_hz`, or `_bytes` where needed.
Keep voice operations distinct from their underlying channel-script adapter
operations, and name queued requests separately from immediate actions.
Hardware helpers may name their peripheral. HAL callbacks, interrupt handlers,
generated entry points, shared Berry APIs, and script-visible names retain their
required names. Update declarations, callers, simulator replacements, tests,
and documentation together when renaming an application function.

The compiler's shared ABI/source snapshot and vendored Berry files must match
this firmware revision. The host and target `berry_conf.h` configurations are
intentionally different. Compile scripts using the compiler bundled with the
same firmware release.

## Hardware constraints

Audio DMA defaults to 48 frames per half-buffer at 48 kHz. USB traffic does not
clock the DAC. I2S DMA buffers use AXI SRAM because DMA1 cannot access DTCM.
The I2S1 master must start before the I2S2 slave. I2S2 requires its existing
underrun handling and IOSWP wiring configuration. Keep these constraints when
maintaining the audio path.

The shared RS485 transceiver requires this card's TX and enable pins to return
to high impedance when idle. Send one command at a time and wait for its reply.
USB bulk throughput and playback latency require hardware measurement; successful
builds and host tests do not establish those timing guarantees.

DAC channels 2–4 supply VCA gain, VCF cutoff, and VCF resonance control voltages
respectively. Startup sets their calibrated 0 V targets before playback,
then slews toward those targets from the first DMA buffer. No diagnostic
tones are generated. Their calibration and slew limits are in
`app/audio_dac.cpp`; there is no console command to set them. The analog
switch defaults are in `core/src/gpio.c` and `app/channel.cpp`.
