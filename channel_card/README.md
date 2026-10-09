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

Install GNU Make, Python 3 (for build validation and reports), and the GNU Arm
embedded toolchain with newlib (`arm-none-eabi-gcc`, `arm-none-eabi-g++`, `arm-none-eabi-objcopy`,
`arm-none-eabi-ar`, and `arm-none-eabi-objdump` on PATH). No build dependencies
are downloaded.

From `channel_card/app/`, run:

```sh
make
```

This builds Release firmware and writes **`bin/channel_MCU.bin`**, beside `app/`.
The standalone `app/Makefile` compiles and links directly, keeping intermediate
files under `build/release/`. CMake and Ninja are not required.
Use `make -j4` to build with up to four parallel jobs.
Use `make clean` to remove this build and its output binary.

Every build prints the firmware size, built baud rate, toolchain, timestamp,
checksum, and Flash/RAM allocation by memory region and section. To view the
last successful build without compiling or changing settings:

```sh
make info
```

The report includes used/free bytes and percentages for Flash, DTCM, AXI SRAM
(`RAM_D1`), `RAM_D2`, `RAM_D3`, and ITCM. Section sizes show sample storage,
the Berry arena, DMA buffers, and the heap/stack reservation. These are linker
allocations, including alignment, rather than measured runtime memory peaks.
The saved report stays under `build/release/`; `bin/` contains only the binary.

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
for your host OS. The script uses its command-line programmer; CubeIDE is not
required. Standard macOS application and Linux installation paths are detected.
For another location, put `STM32_Programmer_CLI` on PATH or set
`CUBE_PROGRAMMER` to the full executable path.

1. Run `make` in `app/` to build the firmware.
2. Run `./flash.sh` from `channel_card/` (or `../flash.sh` from `app/`).
3. The script waits for USB bootloader mode. Connect the USB data cable, move
   the BOOT switch up, and press reset. Press Ctrl+C to cancel the wait.
4. After successful verification, move BOOT down and press reset to run.

Flashing uses the existing `bin/channel_MCU.bin` at `0x08000000`; it does not
rebuild or change its baud rate. The script stops with an explanation if the
programmer or binary is missing, USB enumeration fails, or writing or
verification fails. With multiple bootloaders connected, identify the Channel
Card's port and use `./flash.sh --port USB2` (substitute its port). This waits
for that port before flashing.
Use `./flash.sh --check` to check the programmer and binary without accessing
the board. The script can be invoked from any directory.

For GUI flashing, select USB in STM32CubeProgrammer, connect to the bootloader,
download `bin/channel_MCU.bin` at `0x08000000`, and enable verification.

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
| `cpuload [0\|1]` | RS485 | Query or enable the LED_Y DMA-refill scope probe. Low is busy; high is idle. |

Send `c:reset\r` and wait for `ok:reset` before sending another RS485 command:
bytes already queued behind reset are discarded. The command must reach a
working console; it cannot reset the host adapter or recover a disconnected bus.
Lifetime RX-drop counters are preserved.

The former ASCII `al`, `wl`, and `vmload` USB operations are replaced by
binary upload kinds 1, 2, and 3. BODY blocks can be interleaved with upload chunks.
Full upload sequencing and reply fields are documented in
[wire protocol](PROTOCOL.md).

### Service diagnostics

`vm mem`, `usb`, `usbdev`, `rs485`, and `cpuload` report runtime or transport state.

With `cpuload 1`, LED_Y/PB9 goes low on entry to each SPI1 DMA half-buffer
callback and high after its 48-frame refill completes. At 48 kHz the period is
1 ms, so scope duty cycle is `low_time / 1 ms`. The pulse includes USB callback
work, pending Berry handlers, and sample/oscillator/filter/envelope mixing.
Use `cpuload 0` to return the fixed LEDs to normal operation.

Pitch-track smoke with a loaded sample: `f0 300`, `fk0 1`, `n0 on 60` then
`n0 on 72` — corner should roughly double with the octave (query `f0`).

## Source layout and maintenance

- `app/`: nine C++ implementations, their headers, and the Release Makefile.
- `berry_runtime/`: first-party script runtime and shared ABI declarations;
  `third_party/berry/` retains the vendored interpreter.
- `core/` and `drivers/`: STM32 initialization, interrupt glue, HAL, and CMSIS.
- `channel_MCU.ioc`, startup assembly, and `STM32H725xG_flash.ld`: hardware and
  memory configuration.

The application is split by responsibility:

| Implementation | Contents | Header |
| --- | --- | --- |
| `audio.cpp` | Calibrated DC outputs and I2S/DMA audio bridge | `audio.h` |
| `cs4304.cpp` | CS4304 DAC register access, initialization, gain and mute | `cs4304.h` |
| `channel.cpp` | RS485 commands, UART5 interrupt, LEDs, script uploads | `channel.h` |
| `voice.cpp` | Note playback, envelopes, Berry adapter | `voice.h` |
| `filter.cpp` | Four-pole kernel and per-voice filter control | `filter.h` |
| `oscillator.cpp` | Wavetable oscillators, routing, interpolation helpers | `oscillator.h` |
| `samples.cpp` | Sample attack storage and playheads | `samples.h` |
| `stream.cpp` | Per-voice BODY rings | `stream.h` |
| `usb.cpp` | CDC device, descriptors, binary protocol, sample uploads | `usb.h` |

Use simple C++17 with plain functions and structs. Exceptions, RTTI, and
thread-safe local-static initialization are disabled. Do not add virtual
functions, STL containers, or dynamic initialization to this application.
Public interfaces use C linkage for generated C and the Berry runtime;
interrupt handlers must also retain C linkage. The interpreter and generated
STM32 sources keep their existing C implementation.

Keep source lists, compiler flags, and link settings in `app/Makefile`.
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

A section can group related small helpers. Comments explain purpose, contracts,
and hardware constraints. Preserve public names, copyright notices, and vendor
formatting. Local tests and development tools are maintained separately from
the SVN package.

The compiler's shared ABI/source snapshot and vendored Berry files must match
this firmware revision. The host and target `berry_conf.h` configurations are
intentionally different. Compile scripts using the compiler bundled with the
same firmware release.

## Hardware constraints

Audio DMA refills 48 frames per half-buffer at 48 kHz. USB traffic does not
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
`app/audio.cpp`; there is no console command to set them. The analog
switch defaults are in `core/src/gpio.c` and `app/channel.cpp`.
