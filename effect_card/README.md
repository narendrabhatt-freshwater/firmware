# Effect Card — Firmware

Standalone checkouts (SVN trunk) ship the toolchain handbook as
[`docs/firmware_handbook.md`](docs/firmware_handbook.md) and the wire
contract as [`docs/protocol.md`](docs/protocol.md). In the git monorepo
those same files live at the repository root (`README.md`,
[`docs/protocol.md`](docs/protocol.md) here and
[`../docs/protocol.md`](../docs/protocol.md)).

| | |
|---|---|
| MCU | STM32H743xx |
| CMake target | `effect_card` |
| CubeMX project | `effect_card.ioc` |
| Linker script | `STM32H743xx_FLASH.ld` |
| USB stack | TinyUSB **0.18** (`ThirdParty/tinyusb-0.18`) |
| USB device | UAC2 microphone (mono, 32-bit, 96 kHz) + CDC console |
| RS485 address | `e:` |

## What this card does

Captures **8 analog channels** from two TLV320ADC6140 ADCs over a shared
TDM bus, and streams **one selectable channel** to the PC as a USB
microphone. Also controls the 48 V phantom rail.

- **ADC1** (I2C `0x4C`) → SAI1 Block B → `u 1..4`
- **ADC2** (I2C `0x4D`) → SAI1 Block A → `u 5..8`
- 48 V rail enable + power-good monitoring
- Console over RS485 (`e:` prefix) and USB CDC

Only one channel goes to USB at a time — USB Full-Speed cannot carry all
eight at 96 kHz/32-bit. Switch channels live with `u <1..8>`; the
change takes effect immediately.

## Build & flash

```bash
cmake --preset Debug
```

```bash
cmake --build build/Debug
```

Then flash `build/Debug/effect_card.hex` over DFU — see
[`docs/firmware_handbook.md`](docs/firmware_handbook.md) in an SVN
checkout, or [`../README.md`](../README.md) §3 in the git monorepo.

## Source map

| Path | Contents |
|---|---|
| `Core/Src/main.c` | Bring-up, **SAI capture → USB**, main loop wiring |
| `Core/Src/console/effect_console.c` | RS485 + USB CDC console, ADC I2C bring-up, LED flash |
| `Core/Src/sai.c` | SAI init + critical TDM overrides (see below) |
| `USB_APP/` | TinyUSB descriptors, `tusb_config.h`, UAC2 + CDC glue |

## Audio capture path — the fragile parts

The chain is: **ADCs → TDM/SAI → circular DMA → de-interleave → TinyUSB
FIFO → USB**. Mermaid of that path (and the Channel card):
[`docs/diagrams/card_data_flow.md`](docs/diagrams/card_data_flow.md).

Several settings here are non-obvious and were established
by measurement. All are commented in-place; the summary:

### `sai.c` — TDM alignment override (USER CODE section)

```c
hsai_BlockA1.FrameInit.FSOffset = SAI_FS_BEFOREFIRSTBIT;
hsai_BlockB1.FrameInit.FSOffset = SAI_FS_BEFOREFIRSTBIT;
```

The TLV320ADC6140 starts its first TDM slot **one BCLK after** the FSYNC
edge. Without this override the SAI samples one bit early, the sign bit
picks up the previous slot's LSB, and audio comes out as ragged
square-wave noise with the real signal buried underneath.

`AudioFrequency` must stay at the generated **96K**. Each 96-frame DMA
half completes every 1 ms at 96 kHz, so half/full callbacks provide one
refill opportunity per millisecond. Doubling `AudioFrequency` drives a
192 kHz bus into the 96 kHz USB stream and overflows the FIFO (dropped
chunks, audible beating).

### `main.c` — cadence must match USB

```c
#define SAI_FRAMES_PER_HALF 96   /* 1 ms at 96 kHz */
```

The DMA half-buffer is deliberately **1 ms**, matching the USB frame
interval, so the producer (SAI callback) and consumer (one iso IN token
per 1 ms) tick together and the FIFO stays near-constant. Using 2 ms
halves made the FIFO swing 192→0 every cycle; any frame landing on the
empty phase got a short packet and an audible dropout.

### `tusb_config.h` — endpoint size is a rate limiter

`CFG_TUD_AUDIO_EP_SZ_IN` must stay at the **computed** value (97 samples
/ 388 B), not a rounder, larger number. TinyUSB sends
`min(FIFO content, EP size)` per token, so the EP size is what limits
the drain. Enlarging it to 128 samples made the drain outpace the
producer (128/64/128/64 instead of a steady 96) — visible as ragged
dropouts even in a constant-amplitude test signal.

### Interrupt priority

USB is deliberately **higher priority than the SAI DMA** (set in
`main.c` after `USB_App_Init()`). Isochronous IN re-arming is
deadline-critical — miss the token, lose the frame — while the SAI
callbacks have a relaxed 1 ms deadline.

### TinyUSB 0.18

This card uses 0.18 for its **dwc2 isochronous-IN fixes**. Earlier dwc2
code missed roughly 8 IN service intervals per second, each one a 1 ms
hole that produced a host-side discontinuity. TinyUSB 0.18 splits shared
dwc2 code into `dwc2_common.c`; keep that source in `CMakeLists.txt`.

## ADC configuration

Both ADCs are initialised automatically at boot (`ADC_Init`) — reset,
wake, **TDM/32-bit ASI format**, enable all four input channels, route
them to the bus, power up. They auto-detect their sample rate from the
incoming BCLK/FSYNC, so changing the SAI rate is enough — no register
changes needed.

`AUDIO_EN` (PA7) releases the ADCs from hardware shutdown and is raised
at boot; they will not answer on I2C while it is low.

## Console command reference

This table matches the parser in `Core/Src/console/effect_console.c`.
Commands are case-insensitive because console input is converted to lowercase.
End a command with carriage return.

On shared RS485, prefix commands with `e:`. USB CDC accepts `e:`, `*:`, or no
prefix. RS485 replies are tagged `[E]`; CDC replies contain only the body.
Setters return `ok` on success. Invalid commands return `err:syntax` or
`err:unknown`; ADC communication failures return an `err:` reply describing
the chip that did not acknowledge.

| Command | Action |
| ------- | ------ |
| `h` / `help` / `?` | Return the live command list. |
| `s` | Return 48 V enable, power-good, audio enable, LED-show state, and RS485 echo state. |
| `v` | Query 48 V enable and power-good. |
| `v 0` / `v 1` | Disable or enable the 48 V phantom rail. |
| `a 0` / `a 1` | Hold the ADC audio domain in shutdown or enable it. Enabling waits for ADC wake-up before replying. |
| `u` | Query the ADC channel currently sent to the USB microphone. |
| `u <channel>` | Select USB capture channel 1…8. |
| `l 0` / `l 1` | Disable or enable the automatic LED show. |
| `lr 0` / `lr 1` | Turn the red LED off or on. Turning it on disables the automatic show. |
| `ly 0` / `ly 1` | Turn the yellow LED off or on. Turning it on disables the automatic show. |
| `ec` | Query RS485 keystroke echo. |
| `ec 0` / `ec 1` | Disable or enable RS485 keystroke echo. Default is off. |
| `i2c` | Scan legal I2C2 addresses and print every responding device. ADC1 and ADC2 should appear at `0x4C` and `0x4D`. |
| `ai` | Re-run the standard initialization sequence on both ADCs. |
| `ar <chip> <reg>` | Read register 0x00…0xFF from ADC chip 1 or 2. Register input is hexadecimal. |
| `aw <chip> <reg> <value>` | Write value 0x00…0xFF to an ADC register. Register and value inputs are hexadecimal. |

The ADC register commands are service operations. Normal applications should
use the high-level `cmi::Core` effect controls and should not depend on ADC
register values. For example, `aw 1 07 3c` changes ADC1 register `0x07` to
`0x3C`.
