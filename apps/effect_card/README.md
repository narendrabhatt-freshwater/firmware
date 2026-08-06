# Effect Card — Firmware

> **Start with [`../../README.md`](../../README.md)** — toolchain setup, build
> commands, DFU flashing and the STM32CubeMX regeneration rules are
> common to both cards and documented there.

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

- **ADC1** (I2C `0x4C`) → SAI1 Block B → `usb ch 1..4`
- **ADC2** (I2C `0x4D`) → SAI1 Block A → `usb ch 5..8`
- 48 V rail enable + power-good monitoring
- Console over RS485 (`e:` prefix) and USB CDC

Only one channel goes to USB at a time — USB Full-Speed cannot carry all
eight at 96 kHz/32-bit. Switch channels live with `usb ch <1..8>`; the
change takes effect immediately.

## Build & flash

```bash
cmake --preset Debug
```

```bash
cmake --build build/Debug
```

Then flash `build/Debug/effect_card.hex` over DFU — see
[`../../README.md`](../../README.md) §3.

## Source map

| Path | Contents |
|---|---|
| `Core/Src/main.c` | Bring-up, **SAI capture → USB**, main loop wiring |
| `Core/Src/console/effect_console.c` | RS485 + USB CDC console, ADC I2C bring-up, LED flash |
| `Core/Src/sai.c` | SAI init + critical TDM overrides (see below) |
| `USB_APP/` | TinyUSB descriptors, `tusb_config.h`, UAC2 + CDC glue |

## Audio capture path — the fragile parts

The chain is: **ADCs → TDM/SAI → circular DMA → de-interleave → TinyUSB
FIFO → USB**. Several settings here are non-obvious and were established
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

There is also a comment recording a **misdiagnosis** — a temporary
"192 kHz" override that appeared to fix a rate problem but actually ran
the bus at double rate. `AudioFrequency` must stay at the generated
**96K**. Do not re-introduce that override.

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

This card runs 0.18 (the Channel Card is on 0.17) for its **dwc2
isochronous-IN fixes**. On 0.17 this core missed roughly 8 IN service
intervals per second, each one a 1 ms hole the host filled with a
discontinuity. 0.17 is kept at `ThirdParty/tinyusb` as a rollback —
switch by editing the paths in `CMakeLists.txt` (0.18 also needs
`dwc2_common.c`, which 0.17 does not have).

## ADC configuration

Both ADCs are initialised automatically at boot (`ADC_Init`) — reset,
wake, **TDM/32-bit ASI format**, enable all four input channels, route
them to the bus, power up. They auto-detect their sample rate from the
incoming BCLK/FSYNC, so changing the SAI rate is enough — no register
changes needed.

`AUDIO_EN` (PA7) releases the ADCs from hardware shutdown and is raised
at boot; they will not answer on I2C while it is low.

## Console quick reference

Type `help` on RS485 (`e:help`) or the USB COM port for the live list.
Frequently used:

| Command | Action |
|---|---|
| `status` | 48 V, LEDs, audio domain state |
| `usb ch <1..8>` | Select which ADC channel streams to USB |
| `48v on\|off` | 48 V phantom rail |
| `i2cscan` | Scan I2C2 (expect `0x4C` and `0x4D`) |
| `adc rd <n> <reg>` | Read ADC register (hex) |
| `adc wr <n> <reg> <v>` | Write ADC register (hex) |
| `audio on\|off` | Audio domain / ADC shutdown |

`adc rd`/`adc wr` are the fast way to experiment with ADC settings
without reflashing — e.g. `adc wr 1 07 3c` flips the BCLK polarity.

