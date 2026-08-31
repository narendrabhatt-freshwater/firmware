# Channel Card — Firmware

Standalone checkouts (SVN trunk) ship the toolchain handbook as
[`docs/firmware_handbook.md`](docs/firmware_handbook.md) and the wire
contract as [`docs/protocol.md`](docs/protocol.md). In the git monorepo
those same files live at the repository root (`README.md`,
[`docs/protocol.md`](docs/protocol.md) here and
[`../docs/protocol.md`](../docs/protocol.md)).

|                |                                                              |
| -------------- | ------------------------------------------------------------ |
| MCU            | STM32H725xG                                                  |
| CMake target   | `channel_MCU`                                                |
| CubeMX project | `channel_MCU.ioc`                                            |
| Linker script  | `STM32H725xG_flash.ld`                                       |
| USB stack      | TinyUSB 0.17 (`ThirdParty/tinyusb`)                          |
| USB device     | UAC2 BODY OUT (10ch int16) + CDC console                     |
| RS485 address  | `c:`                                                         |

## What this card does

Receives USB audio from the PC into **per-voice sustain rings**, and
plays the 8-voice SAMPLE / note bank out of a **CS4304 4-channel DAC**
over I2S.

- **CH1** — SAMPLE note-bank mix (`n0..n7`). UAC2 BODY transport fills the
  per-voice rings used for sustain; it is not mixed onto CH1 as PCM.
- **CH2–CH4** — firmware-generated DC control voltages, clocked purely
  by I2S with no USB involvement (0 V at boot)
- Console over RS485 (`c:` prefix) and USB CDC

The note bank has no firmware-owned envelope policy. After each reset, upload a
valid Channel Berry ABI6 program with Protocol's `fw_vmc` tool before sending note
commands. Until then the card stays silent and replies `err:no-program`.

### USB streaming profile (default)

Normal Debug and Release builds use USB BODY streaming with a 12,240-sample
ring per voice. The `fw build` command and the CMake presets explicitly select
this profile with `CHANNEL_TEST_WAVETABLE=OFF`.

The optional wavetable test profile generates one 128-sample Q15 sine table
during card initialization and disables attack uploads and USB BODY writes.
Enable it only for a direct CMake test build:

```sh
cmake --preset Release -DCHANNEL_TEST_WAVETABLE=ON
cmake --build --preset Release
```

The normal profile keeps two 4,080-sample base banks plus one 4,080-sample tail
bank per voice (12,240 int16 samples, or 24,480 bytes per voice).

## Audio signal path

Firmware / USB / playhead / I2S (including ISR vs main loop):
[`docs/diagrams/card_data_flow.md`](docs/diagrams/card_data_flow.md).

Analog wet/dry and GPIO switches:

![Channel Card audio flow](docs/diagrams/channel_card_audio_flow.jpg)

Green = audio, dashed red = control, tan boxes = analog switches driven
by GPIO. Every tan box maps to one entry in the `switches[]` table in
`Core/Src/console/channel_console.c` (boot defaults; no runtime command).

### DAC channel roles

The CS4304S is one 4-channel DAC doing two different jobs — **one audio
channel and three control voltages**:

| DAC ch | Role                                                | Set with                              |
| ------ | --------------------------------------------------- | ------------------------------------- |
| CH1    | **Audio** — SAMPLE note-bank mix (`n0..n7`)         | `g 1 <dB>`                            |
| CH2    | **CV → VCA** gain                                   | `Audio_SetDCLevel(2, …)` (boot: 0 V)  |
| CH3    | **CV → VCF cutoff**                                 | `Audio_SetDCLevel(3, …)` (boot: 0 V)  |
| CH4    | **CV → VCF resonance**                              | `Audio_SetDCLevel(4, …)` (boot: 0 V)  |

So CH2–CH4 are not heard directly: they steer the analog blocks. The
CV levels are driven through `audio_tone_dc.c` (per-channel zero
calibration, slew limiting); there is currently no console command for
them — firmware sets 0 V at boot.

### The two output routes

Audio from CH1 reaches the output by either — or both — of:

- **Dry:** `bypass` switch → straight to `out`
- **Wet:** through SCF and/or VCF → **VCA** → `vca` switch → `out`

For bring-up/verification, first upload a Channel VM program to voice 0, then
use the note bank: `n0 on 69` plays A4 onto CH1 with the default tuning. Bypass
is already ON from the boot session defaults (bare `n0` re-applies
them), so the tone is heard clean and filter-free at `out`. `n off`
silences all voices.

### Switch reference

| Switch    | Diagram block        | Function                                  | Polarity        |
| --------- | -------------------- | ----------------------------------------- | --------------- |
| `bypass`  | bypass (dry → out)   | Route unprocessed CH1 to the output      | active-low      |
| `scf`     | scf sw (post-filter) | Pass the SCF output on to the VCA        | active-low      |
| `hp_ctl`  | mux 2:1 select       | SCF input: HP stage (on) or direct (off) | **active-high** |
| `vcf`     | vcf path enable      | Feed CH1 into the VCF block              | active-low      |
| `lp`      | vcf → lp             | Take the VCF **low-pass** tap            | active-low      |
| `bp`      | vcf → bp             | Take the VCF **band-pass** tap           | active-low      |
| `hp`      | vcf → hp             | Take the VCF **high-pass** tap           | active-low      |
| `vca`     | vca sw (wet → out)   | Route the VCA (wet) output to `out`      | active-low      |

`hp_ctl` is the one **active-high** switch — see the `switches[]` table
in `Core/Src/console/channel_console.c`, where its `active_low` field is
`0` while every other entry is `1`. All switches are driven OFF at init,
then the session defaults turn `bypass` ON; the polarity only matters if
you drive the GPIOs directly.

The VCF taps (`lp`/`bp`/`hp`) are separate switches, not a selector —
enabling more than one sums those responses into the VCA.

### SCF clock

The SCF's `lp core` cutoff is **clock ÷ 100** (a 100 kHz clock gives a
1 kHz cutoff). The clock line is `filter_ctl` (TIM3_CH1, PC6); firmware
currently holds it LOW at boot and exposes no console command for it —
the former `scf`/`duty` commands were removed with the tone/DC console
surface.

## CPU load probe (yellow LED)

**What you do**

1. Flash the Channel Card build.
2. On RS485/USB console: `cpu` or `cpu 4` (voice count).
3. Probe the **yellow LED** pin — **PB9 / `LED_Y`** (not the red LED).
4. On the scope: **low = CPU busy**, **high = idle**.
5. `CPU% ≈ t_low / (t_low + t_high)`.
6. When done: `cpu 0`.

Smoke: `cpu 1`, `cpu 8`, `cpu 0`, `n0 440 0.5`, `g 1 0` — LED chaser resumes after `cpu 0`.

Bare `cpu` starts all **8** voices (220, 260, … Hz). Pass **`1..8`** for count.

I2S1 half-buffer sample fill runs in the **I2S DMA half/full callbacks**.
`Audio_I2S1_Poll()` remains an empty compatibility hook. Bare `cpu` /
`cpu N` measures NoteBank fill busy-time on LED_Y around the callback refill.

| Command             | Action                                           |
| ------------------- | ------------------------------------------------ |
| `cpu` / `cpu N`     | N voices (default 8) + DMA-callback fill probe   |
| `cpu q` / `cpu q N` | Soft-queue LED probe                             |
| `cpu 0`             | Clear notes, stop probe, resume LED chaser       |

### Load check: 8 voices + 8 filters @ 48 kHz

Sample rate is compile-time (`AUDIO_SAMPLE_RATE_HZ` in `Core/Inc/audio/audio_rate.h`,
currently **48 kHz**). Flash this build, then:

```text
f 20000                 # all f0..f7 bypass first
f0 300
f1 300
f2 300
f3 300
f4 300
f5 300
f6 300
f7 300                  # LPF on all 8 voices
cpu                     # all 8 voices
```

Scope LED_Y duty as above. Console must still ACK (`cpu 0`, `f`).
At `f 20000` + `cpu` you get the 8-voice / filter-bypass baseline.

## Build & flash

```bash
cmake --preset Debug
```

```bash
cmake --build build/Debug
```

Then flash `build/Debug/channel_MCU.hex` over DFU — see
[`docs/firmware_handbook.md`](docs/firmware_handbook.md) in an SVN
checkout, or [`../README.md`](../README.md) §3 in the git monorepo.

## Source map

Hand-written modules live under `Core/Src/<domain>/` (and matching
`Core/Inc/<domain>/`). CubeMX-generated files stay flat in `Core/Src` /
`Core/Inc`.

| Path                                       | Contents                                                          |
| ------------------------------------------ | ----------------------------------------------------------------- |
| `Core/Src/main.c`                          | Bring-up, DAC init, main loop wiring                              |
| `Core/Src/console/channel_console.c`       | RS485 + USB CDC console, `cpu`, LED chaser                        |
| `Core/Src/console/uart5_rx.c`              | Interrupt-driven UART5 RX ring buffer                             |
| `Core/Src/audio/audio_bridge.c`            | USB → per-voice stream rings; CH1 note-bank mix; I2S DMA |
| `Core/Src/audio/note_bank.c`               | n0–n7 8-voice SAMPLE bank with fallback oscillators               |
| `Core/Src/filters/note_filter.c`           | Per-voice LPF wrapper (base/effective cutoff, pitch-k, q/Q31)     |
| `Core/Src/filters/butterworth_four_pole.c` | Reusable 4-pole DF4 Butterworth kernel                            |
| `Core/Src/drivers/cs4304.c`                | CS4304 DAC driver (I2C)                                           |
| `USB_APP/`                                 | TinyUSB descriptors, 10ch int16 UAC2 BODY + CDC                   |

### `audio_bridge.c` — handle with care

This file holds the playback path as measured on the board. Notable
parts, all commented in-place:

- **BODY stream** is class-compliant synchronous UAC2 OUT: a 10-channel
  signed-int16 51 kHz carrier (1020 bytes/ms) for 48 kHz BODY/DAC data. PACKs
  form one continuous CRC32-protected byte stream; exact free-space `vq`
  permission arrives every
  5 ms. A primed BODY underrun increments `hold`; it never disables USB,
  RS485, or the audio interrupts, and a later authorized refill can recover.
  CH1 I2S is always the
  note-bank mix. The USB IRQ re-arms UAC OUT; the main loop parses BODY.
- **I2S start order matters** — the I2S1 master must be running before
  the I2S2 slave is enabled, or the slave never shifts.
- **I2S2 slave workarounds** — UDR wedge clearing via the TIM7 pump, and
  `CFG2.IOSWP` to swap MISO/MOSI because the board wires PC1 to the
  DAC's SDIN2.
- **DMA buffers live in AXI SRAM** (`.dma_buffer` section) — DMA1 cannot
  reach the DTCM RAM where `.bss` normally lands.

## Volume control

There is no USB speaker volume control. BODY samples pass at unity.
Use **`g <ch> <dB>`** for CS4304 DAC trim.

**`n0`** applies **bypass ON** and **`g 1 0`** (0 dB CH1 DAC trim) at
boot and on bare `n0`. **`n0`…`n7`** are 8 independent phase-accumulator
voices summed onto CH1. Optional **`[scale]`** (0.0..1.0, default **0.125**)
sets that note’s amplitude. When the assigned wave has no loaded attack head,
**`s`** (sine, default), **`p <0.1..0.9>`** (pulse duty), and
**`t <0.1..0.9>`** (triangle asymmetry; 0.5 = symmetric) select its fallback
oscillator. These shape commands do not affect SAMPLE playback from a loaded
attack head and BODY stream. Frequency, scale, and shape changes do not
touch gain or bypass.
**`g`** changes DAC atten on any channel.

## Console quick reference

Type `h` / `help` / `?` on the card for the live list.

| Command                                   | Action                                                                                                                                                                                                                             |
| ----------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `h`                                       | Command list                                                                                                                                                                                                                       |
| `n0`                                      | Session defaults: bypass on, `g 1 0`                                                                                                                                                                                               |
| `n0`…`n7 on <key> [@session]`              | Start raw MIDI key 0…127; the loaded script maps and tunes it                                                                                                                                                                   |
| `n0`…`n7 off`                           | Turn that note off                                                                                                                                                                                                                 |
| `n off`                                   | Silence all 8 voices                                                                                                                                                                                                               |
| `s`                                       | Fallback oscillator shape: sine (voices without a loaded attack)                                                                                                                                                                  |
| `p <0.1..0.9>`                            | Fallback oscillator shape: pulse (duty)                                                                                                                                                                                            |
| `t <0.1..0.9>`                            | Fallback oscillator shape: triangle (asymmetry)                                                                                                                                                                                    |
| `f0`…`f7` `<Hz>` `[q]` / `f` `<Hz>` `[q]` | LPF **base** cutoff at C4 on voices **0..7** (20..20000; **`0` or `20000` = bypass**). Optional **`q`** = DF4 **g** **0.5..10** (default **1.0**). See [`docs/reference/note_filter_butterworth.md`](docs/reference/note_filter_butterworth.md). |
| `fk0`…`fk7` `[k]` / `fk` `[k]`            | Filter pitch-track **k** (0..10, default **0**): `fc = fbase × (note/C4)^k`.                                                                                                                                                         |
| `al <id> <nbytes>`                        | CDC upload: int16 attack head for wave `<id>` 0…255 (2…1024 bytes; join at committed length)                                                                                                                                        |
| `vmload <v> <nbytes>` / `vm [v]` / `vm mem` | CDC upload one ABI6 FWSC Berry program (maximum 4 KiB payload) to voice 0…7 / status / shared-arena diagnostics                                                                                                                    |
| `ar <id> <Hz>` / `aw <v> <id>` / `a`      | Root pitch of head `<id>` / assign head to voice 0…7 / loaded count + hex mask                                                                                                                                                      |
| `vq`                                      | Active mask + hungriest voice + free-slot codes 0–14; 15 means empty                                                                                                                                                               |
| `g <ch> <dB>`                             | DAC atten 0..127 dB on ch 1..4                                                                                                                                                                                                     |
| `cpu` / `cpu N`                           | N voices + LED_Y DMA-callback fill load probe (default 8)                                                                                                                                                                          |
| `cpu q` `[N]`                             | Soft-queue load probe                                                                                                                                                                                                              |
| `cpu 0`                                   | Clear notes; resume LED chaser                                                                                                                                                                                                     |

Shape smoke with a loaded script for n0 (scope on CH1): `n0 on 69`, then
`s`, `p 0.5`, `t 0.5`, or `saw`.
`p 0.5`, `t 0.5`, `p 0.1`, then `f0` sweep — pulse/tri should show
harmonics; LPF still responds.

Pitch-track smoke: `f0 300`, `fk0 1`, `p 0.5`, `n0 on 60` then
`n0 on 72` — corner should roughly double with the octave (query `f0`).
