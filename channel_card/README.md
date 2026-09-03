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
valid Channel Berry ABI6 program with `cmi::Core` before sending note commands.
Until then the card stays silent and replies `err:no-program`.

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

Smoke: load scripts, then run `cpu 1`, `cpu 8`, and `cpu 0`. The LED chaser
resumes after `cpu 0`.

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
boot and on bare `n0`. **`n0`…`n7`** are eight independent voices summed
onto CH1. Their uploaded scripts control tuning and amplitude. When an
assigned wave has no loaded attack head, `s`, `saw`, `p`, and `t` select the
fallback oscillator. Shape changes do not affect sample playback, gain, or
bypass. `g` changes DAC attenuation on any channel.

## Console command reference

This table matches the parser in
`Core/Src/console/channel_console.c`. Commands are case-insensitive because
console input is converted to lowercase. End a command with carriage return.

On shared RS485, prefix commands with `c:`. USB CDC accepts `c:`, `*:`, or no
prefix. RS485 replies are tagged `[C]`; CDC replies contain only the body.
Successful setters normally return `ok`. Common failures are `err:syntax`,
`err:range`, `err:unknown`, `err:no-program`, and `err:vm-busy`.

### Playback and card control

| Command | Action |
| ------- | ------ |
| `h` / `help` / `?` | Return the live command list. |
| `n0` | Apply session defaults: analog bypass on and CH1 trim at 0 dB. It does not start a note. |
| `n0`…`n7 on <key>` | Start voice 0…7 using MIDI key 0…127. A valid script must already be loaded for that voice. |
| `n0`…`n7 on <key> @<session>` | Start a streamed note and bind BODY session 0…254 before acknowledging. |
| `n0`…`n7 off` | Release one voice. |
| `n off` | Release all eight voices. |
| `s` | Select the sine fallback oscillator. |
| `saw` | Select the sawtooth fallback oscillator. |
| `p <duty>` | Select the pulse fallback oscillator; duty range 0.1…0.9. |
| `t <asymmetry>` | Select the triangle fallback oscillator; range 0.1…0.9, with 0.5 symmetric. |
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

Pitch tracking uses `fc = fbase × (noteHz / 261.625565)^k`. See
[`docs/reference/note_filter_butterworth.md`](docs/reference/note_filter_butterworth.md).

### Samples, scripts, and streaming

| Command | Transport | Action |
| ------- | --------- | ------ |
| `al <id> <nbytes>` | USB CDC only | Begin an attack-head upload for ID 0…255. The byte count must be even and 2…1024. After `ok:ready`, send exactly that many signed int16 little-endian bytes; completion returns `ok:attack <id>`. |
| `ar <id> <Hz>` | RS485 or CDC | Set the positive root frequency for attack ID 0…255. |
| `aw <voice> <id>` | RS485 or CDC | Assign attack ID 0…255 to voice 0…7. |
| `a` | RS485 or CDC | Query loaded attack count and the 256-bit loaded mask. |
| `vmload <voice> <nbytes>` | USB CDC only | Begin an FWSC ABI6 program upload to voice 0…7. Total container size is 20…4116 bytes. After `ok:ready`, send exactly that many bytes. |
| `vm` | RS485 or CDC | Query the active-program voice mask. |
| `vm <voice>` | RS485 or CDC | Query active state, target, ABI version, and fault for voice 0…7. |
| `vm mem` | RS485 or CDC | Return shared VM arena and per-voice fault/cycle diagnostics. |
| `vq` | RS485 or CDC | Query active/pending masks, BODY sessions, target fill, and exact writable credit. RS485 uses the fixed binary-compatible `vq7` response. |
| `usb` | RS485 or CDC | Query BODY transport and underrun counters. |
| `usb 0` | RS485 or CDC | Clear BODY transport counters and return the new values. |

`al` and `vmload` switch the CDC connection from line parsing to binary input
until the declared byte count has arrived. Do not send another command during
that payload. Full upload sequencing and reply fields are documented in
[`../docs/protocol.md`](../docs/protocol.md).

### Service diagnostics and compatibility responses

| Command | Action |
| ------- | ------ |
| `cpu` / `cpu <N>` | Start the DMA load probe with 8 voices or N voices, where N is 1…8. |
| `cpu q` / `cpu q <N>` | Start the soft-queue load probe with 8 voices or N voices. |
| `cpu 0` | Stop the probe, release notes, and resume the LED chaser. |
| `bl ...` | Retired BODY upload command; always returns `err:unsupported`. BODY is streamed over UAC2. |
| `fb [..]` | Retired asynchronous-feedback command; always returns `err:unsupported`. |

`cpu`, `vm mem`, `usb`, `bl`, and `fb` are service/compatibility commands;
applications should use the high-level `cmi::Core` operations instead.

Shape smoke with a loaded script for n0 (scope on CH1): `n0 on 69`, then
`s`, `p 0.5`, `t 0.5`, or `saw`.
`p 0.5`, `t 0.5`, `p 0.1`, then `f0` sweep — pulse/tri should show
harmonics; LPF still responds.

Pitch-track smoke: `f0 300`, `fk0 1`, `p 0.5`, `n0 on 60` then
`n0 on 72` — corner should roughly double with the octave (query `f0`).
