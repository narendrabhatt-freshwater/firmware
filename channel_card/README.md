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

From `channel_card/app/`, plain `make` builds the STM32 firmware at
`bin/channel_MCU.bin`; `make board` is an explicit alias.

The shared application is in [`channel/`](../channel/). Run plain `make` there
using its lowercase `makefile`, then `./bin/simulator`. PortAudio is required;
MacPorts/Homebrew installations are detected, or set `PORTAUDIO_PREFIX`.
The simulator runs the same Berry, voices and protocols as STM32. It prints
USB/RS485 PTY paths and starts silent. Compile programs using the separate Berry
compiler, upload them over binary USB, and send note commands over RS485.

To build the STM32 firmware, install GNU Make, a POSIX shell with standard tools
(`awk`, `cmp`, `shasum`), and the GNU Arm
embedded toolchain with newlib (`arm-none-eabi-gcc`, `arm-none-eabi-g++`, `arm-none-eabi-objcopy`,
`arm-none-eabi-ar`, and `arm-none-eabi-objdump` on PATH). No build dependencies
are downloaded. Python is not required. For systems with `sha256sum`, use
`make board SHA256=sha256sum`.

From `channel_card/app/`, run:

```sh
make board
```

This builds Release firmware and writes **`bin/channel_MCU.bin`**, beside `app/`.
The standalone `app/Makefile` compiles and links directly, keeping intermediate
files under `build/release/`. CMake and Ninja are not required.
Use `make board -j4` to build with up to four parallel jobs.
Use `make clean` to remove the board build and its output binary;
the simulator uses `make -C channel clean` from the repository root.

Builds print short stage messages plus compiler warnings and errors. Run
`make report` for firmware size, baud rate, toolchain, checksum, and Flash/RAM
allocation by region and section. To read the last successful report without
compiling or changing settings:

```sh
cat ../build/release/channel_MCU.info
```

The Makefile handles validation and reporting directly. The firmware package
contains no Python tools, shell-script files, or test harnesses.

The report includes used/free bytes and percentages for Flash, DTCM, AXI SRAM
(`RAM_D1`), `RAM_D2`, `RAM_D3`, and ITCM. Section sizes show sample storage,
the Berry arena, DMA buffers, and the heap/stack reservation. These are linker
allocations, including alignment, rather than measured runtime memory peaks.
The saved report stays under `build/release/`; `bin/` contains the firmware binary.
The SVN package contains the STM32 firmware sources: use `make` or `make board`
from `app/`. The package also includes `channel/` with the shared application,
Berry runtime and desktop simulator. Its board Makefile points at that packaged
location; the development build points at the sibling `channel/` directory.

The RS485 rate is set by `BAUDRATE ?= 921600` at the top of
[`app/Makefile`](app/Makefile). Edit that value for the normal build rate,
or override it for a build:

```sh
make board BAUDRATE=3000000
```

Changing the rate rebuilds the affected code automatically. `make board`
returns to the Makefile's value. Configure the host to use the same rate,
with 8 data bits, no parity, and 1 stop bit (8N1).

## Flash

Install [STM32CubeProgrammer](https://www.st.com/en/development-tools/stm32cubeprog.html)
for your host OS; CubeIDE is not required.

1. Run `make board` in `app/` to build the firmware.
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
Compile a program with the bundled Berry compiler and upload it to each voice
that will be used using the matching binary USB protocol. Upload the sample attack and
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
`channel/channel.cpp`. Commands are case-insensitive because
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
| Upload kind 3 | Binary USB | Upload an FWSC ABI2 program to voice 0…7. Send one header and 20…16404 data bytes; one final reply confirms validation/commit. |
| `vm` | RS485 | Query the active-program voice mask. |
| `vm <voice>` | RS485 | Query active state, target, ABI version, and fault for voice 0…7. |
| `vm mem` | RS485 | Return shared VM arena and per-voice fault/cycle diagnostics. |
| `vq` | RS485 | Query the active mask, processed BODY sequence, playing sessions, free samples and five-ms demand in the fixed 56-byte reply. |
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

- `channel/` (a sibling of `channel_card/` in development): flat shared application
  `.cpp`/`.h` files, desktop `main.cpp`, `hardware.h` and lowercase `makefile`.
- `channel/berry_runtime/`: runtime and ABI declarations, with the vendored
  interpreter under `third_party/berry/`.
- `app/`: STM32 audio rings, DMA, RS485, USB hardware, DAC drivers and build rules.
- `core/` and `drivers/`: STM32 initialization, interrupt glue, HAL, and CMSIS.
- `channel_MCU.ioc`, startup assembly, and `STM32H725xG_flash.ld`: hardware and
  memory configuration.
- `berry_compiler/`: separate host compiler, never linked into the simulator.

The application is split by responsibility:

| Implementation | Contents | Header |
| --- | --- | --- |
| `channel/audio.cpp` | Audio diagnostics | `audio.h` |
| `app/audio.cpp` | STM32 output rings, shared rendering and CV packing | `audio_buffers.h` |
| `app/audio_dac.cpp` | Calibrated control voltages and DAC slot packing | `audio_output.h` |
| `transport.cpp` | Nonblocking RS485 reply queue | `transport.h` |
| `audio_i2s.cpp` | I2S/DMA buffers and callbacks, I2S2 underrun guard | `audio_output.h` |
| `board_stm32.cpp` | UART5, RS485 pins, board outputs, clock, synchronization and profiling | `board.h`, `serial_buffer.h`, `board_stm32.h` |
| `cs4304.cpp` | CS4304 DAC register access, initialization, gain and mute | `cs4304.h` |
| `channel/channel.cpp` | Application initialization, mono rendering, commands and uploads | `channel.h` |
| `platform.cpp` | Execution synchronization, with optional platform operations | `platform.h` |
| `voice.cpp` | Note playback, envelopes, Berry adapter | `voice.h` |
| `filter.cpp` | Four-pole kernel and per-voice filter control | `filter.h` |
| `oscillator.cpp` | Wavetable oscillators, routing, interpolation helpers | `oscillator.h` |
| `samples.cpp` | Sample attack storage and playheads | `samples.h` |
| `stream.cpp` | Per-voice BODY rings | `stream.h` |
| `usb.cpp` | Binary protocol, sample uploads | `usb.h` |
| `usb_device.cpp` | CDC device on HAL PCD, descriptors, transfer queues | `serial_buffer.h`, `usb_device.h` |

The board Makefile lists portable sources in `SHARED_APP_SOURCES` and adds
hardware sources in `APP_SOURCES`. Development host tests reuse the shared list.
Only `audio_i2s.cpp`, `board_stm32.cpp`, `usb_device.cpp`, and `cs4304.cpp`
access STM32 registers or HAL. Shared sources use `platform.h` and `transport.h`;
the host build needs no STM32 headers. Each build supplies `hardware.h`
through its include path to define buffer counts and storage attributes:
`core/inc/` defines STM32 RAM sections, while development
test support defines host alignment. Application sources use the
same storage declarations without platform-selection conditionals.

`init_application()` initializes filters, envelopes, sample storage, streams,
voices and Berry in the existing order. `generate_samples(int32_t *buffer)` begins
one voice block, mixes exactly 48 mono samples, and finishes the block. It owns
no output storage or scheduling state. Startup remains silent until a program
and notes are supplied.

STM32's `app/audio.cpp` owns the two output rings in DMA-accessible AXI SRAM.
Its `generate_audio_samples()` wrapper calls the shared renderer, interleaves
mono audio and calibrated control voltage, and publishes complete slots. Read
and write positions wrap at twice `BUF_COUNT`; the default is two 48-frame slots
per output. DMA consumes them directly and wakes main on completion. Its control
voltage handling and underrun policy are unchanged.

The desktop platform owns a separate 30-slot mono ring. It initializes the
application and prefills 29 slots before starting PortAudio with an unspecified
callback size. Each callback copies available samples, keeps the offset of a
partially consumed block, and posts a zero-byte token for each completed block.
It never waits for rendering. If the ring runs empty, the existing callback
fills the remaining output with silence.

The simulator main loop waits in `select()` for the request pipe and both PTYs.
An audio token renders one replacement block. The pipe carries only audio
requests. The core services pending parsers and replies on receipt of bytes and
before rendering each block; there are no additional scheduling helpers in main.
`channel.h` retains its original const-vector, `void` receive/write functions,
with only `write_usb()` added. The platform passes each received chunk once and
reports the written byte count through `sent_serial()` or `sent_usb()`.
`channel.cpp` owns pending input, feeds the existing bounded parser buffers,
and retains unsent output. Partial writes retry on later application passes.
Berry, parsers and mixing run on main. Protocol deadlines use monotonic platform
milliseconds, while audio advances its own sample clock. DAC calibration and
DMA stay in `app/`.

USB protocol framing, uploads, BODY credits and replies are shared. RS485 uses
the existing bounded reply queue and pauses parsing while replies are pending.
STM32 drivers still own interrupt queues, USB lifecycle events, and RS485 bus
access/deadlines. Simulator PTYs stay open for the process lifetime; opening or
closing a client does not emulate USB connection-generation changes. A protocol
fault retains the existing policy of waiting for a lifecycle reset.

The firmware main loop remains:

```cpp
wait_interrupt();
process_rs485();
process_usb();
generate_audio_samples();
```

Firmware's counting wakeup is signaled by DMA, UART, USB and SysTick; the tick
also lets main check protocol deadlines. Generic synchronization defaults to
single-threaded execution; STM32 binds its existing interrupt preservation and
memory barriers. `CHANNEL_ENABLE_CYCLE_PROFILING` enables firmware measurements.
The gain command's existing blocking DAC I2C writes and actual STM32 timing need
separate board measurements; host tests do not validate physical deadlines.

`init_board_platform()` binds STM32 synchronization and physical colour/gain
handlers. `init_channel_board()` initializes
physical outputs and RS485 reception in their existing order, before
`init_application()` initializes shared application state and queues ready. DMA starts after this
initialization so startup work cannot consume its first refill interval.
Firmware main calls `init_usb_device()` directly for the custom CDC device setup.
The USB parser starts initialized and follows connection-generation changes in
`process_usb_messages()`; the platform calls this through the buffer handler.

`board_stm32.cpp` owns LED patterns, GPIO mapping, RGB quantization and PWM.
Shared scripts and commands make logical colour/gain requests through optional
`ChannelOutputs` handlers. Without handlers these requests retain their existing
headless acceptance behavior. The I2S driver supplies optional output diagnostics;
without a driver, overlap counts are zero. RS485 bus control is private to STM32.

Use simple C++17 with plain functions and structs in the shared application and
firmware. Simulator `main.cpp` also uses C++17.
Exceptions, RTTI, and
thread-safe local-static initialization are disabled. Do not add virtual
functions, STL containers, or dynamic initialization to this application.
Public interfaces use C linkage for generated C and the Berry runtime;
interrupt handlers must also retain C linkage. The interpreter and generated
STM32 sources keep their existing C implementation.

Keep shared and hardware application source lists in `app/Makefile` up to date;
it also owns compiler flags and link settings.
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

Put a descriptive banner above every function. Start every application and simulator
source, header, test, and build file with the standard Freshwater ASCII/copyright
header from `channel/audio.cpp`. Use `#` comments for headers and section dividers in
scripts and Makefiles, keeping script shebangs first. Comments explain contracts
and hardware constraints that the code cannot express. Preserve Freshwater file
headers, copyright notices, and vendor formatting. Local tests and development
tools are maintained separately from the SVN package.

Function names follow MAS: lowercase words separated by underscores, describing
the operation and its object. Use `process_rs485_commands()`, `generate_audio_samples()`,
and `read_serial_input()` in shared code.
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

Audio DMA defaults to 48 frames per block at 48 kHz. USB traffic does not
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
switch defaults are in `core/src/gpio.c` and `channel/channel.cpp`.
