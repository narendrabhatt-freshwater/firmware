# Freshwater CMI — Firmware and Host Tools

Common quick-start for **both** cards. Per-card detail lives in each
project's own `README.md`; the host↔card wire contract lives in
[`docs/protocol.md`](docs/protocol.md).

| Card         | Folder               | MCU         | CMake target  | CubeMX file       |
| ------------ | -------------------- | ----------- | ------------- | ----------------- |
| Channel Card | `channel_card/` | STM32H725xG | `channel_MCU` | `channel_MCU.ioc` |
| Effect Card  | `effect_card/`  | STM32H743xx | `effect_card` | `effect_card.ioc` |

Channel Card includes HAL, CMSIS and Berry; its custom CDC layer uses HAL PCD.
Effect Card retains TinyUSB.
Effect Card no longer links the unused VM library. The old host library and
GUI have been retired.
Use [`voice_bd`](voice_bd/README.md) for standalone playback or the application
in `175-mainframe`.

To build the Berry script compiler on a teammate's computer, see
[`berry_compiler/`](berry_compiler/README.md). It is a separate host build and
does not require the STM32 toolchain.

---

## 1. Tools to install

| Tool                    | Why                                                       | Notes                                                   |
| ----------------------- | --------------------------------------------------------- | ------------------------------------------------------- |
| **STM32CubeCLT**        | CMake + Ninja + `arm-none-eabi-gcc` toolchain             | Provides the whole build chain. Easiest single install. |
| **STM32CubeMX**         | Regenerating the HAL/peripheral framework from the `.ioc` | Only needed if you change pinout/peripherals            |
| **STM32CubeProgrammer** | Flashing over USB DFU                                     | Also installs the DFU USB driver                        |

STM32CubeIDE bundles equivalents under
`…/AppData/Local/stm32cube/bundles/` (`cmake/`, `ninja/`,
`gnu-tools-for-stm32/`) if you already have it installed.

**PATH check** — the toolchain `bin` directory must be on `PATH`, in
particular `arm-none-eabi-objcopy`. The final build step converts
`.elf` → `.hex`/`.bin` with it; if it is missing you get a successful
link followed by:

```
'arm-none-eabi-objcopy' is not recognized as an internal or external command
```

Everything else built fine at that point — only the `.hex`/`.bin`
conversion failed.

---

## 2. Building

From inside a project folder (`channel_card/` or `effect_card/`):

```bash
cmake --preset Debug
```

```bash
cmake --build build/Debug
```

Presets available: `Debug`, `Release` (see `CMakePresets.json`).
Generator is Ninja; the toolchain file is
`cmake/gcc-arm-none-eabi.cmake`.

Artifacts land in `build/Debug/`:

- `<target>.elf` — for debugging / CubeProgrammer
- `<target>.hex` — for DFU flashing
- `<target>.bin` — raw binary

A clean rebuild is just `rm -rf build/` then re-run the two commands.

VS Code with the STM32 / CMake Tools extensions picks up the presets
automatically if you prefer a GUI.

---

## 3. Flashing over USB DFU

Both boards boot to the built-in ST DFU bootloader from USB — no
ST-LINK required.

1. **Slide the BOOT toggle switch UP.**
2. **Press the reset pushbutton.**
3. The board enumerates as **"STM32 BOOTLOADER"** (DFU device).
4. Open **STM32CubeProgrammer** → select **USB** → refresh the port →
   **Connect**.
5. **Open file** → pick `build/Debug/<target>.hex` (or `.elf`) →
   **Download**.
6. **Slide the BOOT toggle back DOWN** and **press reset** to run the
   new firmware.

If the DFU device does not appear: confirm the toggle is up *before*
pressing reset, try a different USB port/cable, and check
Device Manager for "STM32 BOOTLOADER" (install the driver bundled with
STM32CubeProgrammer if it is flagged).

---

## 4. Regenerating from STM32CubeMX — read this first

The `.ioc` is the source of truth for pins, clocks and peripheral init.
Regeneration **overwrites generated files**, so respect these rules or
you will silently lose working code.

### 4.1 Only write inside USER CODE sections

```c
/* USER CODE BEGIN 2 */
   ← your code goes here; survives regeneration
/* USER CODE END 2 */
```

Anything outside these markers is regenerated and lost. All custom code
in both projects already lives inside them.

### 4.2 Never add hand-written sources to the generated CMake file

`cmake/stm32cubemx/CMakeLists.txt` is **regenerated from the `.ioc`**.
Any source you add there disappears on the next regeneration.

Add hand-written sources to the **top-level `CMakeLists.txt`** instead,
under `target_sources(${CMAKE_PROJECT_NAME} PRIVATE …)`. This is already
done for e.g. `Core/Src/drivers/cs4304.c` and
`Core/Src/audio/audio_bridge.c` on the Channel Card — both were dropped
by a regeneration once, which is why they now live in the top-level file.

### 4.3 `Middlewares/` belongs to CubeMX — third-party code goes in `ThirdParty/`

CubeMX manages and prunes `Middlewares/`. Effect Card TinyUSB lives in
**`ThirdParty/`**, which CubeMX does not touch. Channel Card USB is custom
application code under `USB_APP/`.

### 4.4 Do not enable the ST USB Device middleware

USB is owned by the custom CDC layer on Channel Card and TinyUSB on Effect Card.
In CubeMX:

- Connectivity → **USB_OTG_xS: Device_Only**
- NVIC → **OTG global interrupt: enabled**
- Middleware → **USB_DEVICE class: Disable**

Do not enable a second USB middleware stack. Channel Card uses the generated
`MX_USB_OTG_HS_PCD_Init()`, custom CDC setup in `USB_App_Init()`, and
`HAL_PCD_IRQHandler()` directly. Effect Card continues using TinyUSB and its
`tud_int_handler()` IRQ routing. Keep hand-written code in USER CODE sections.

### 4.5 Post-regeneration checklist

After every regeneration, verify these survived (they are all in USER
CODE blocks, but check anyway):

- [ ] `USB_App_Init()` called in `main()`
- [ ] `USB_App_Task()` called in the main `while(1)` loop
- [ ] HAL PCD IRQ handler on Channel; `tud_int_handler(0)` on Effect
- [ ] Hand-written sources still listed in the **top-level** `CMakeLists.txt`
- [ ] Project builds and the board still enumerates over USB

---

## 5. Repository layout (same shape in both projects)

The firmware projects live at the repo root. Channel Card's interpreter and VM
support live in `channel_card/runtime/`. `berry_compiler/` builds the standalone
host compiler; `voice_bd/` is the standalone playback application. `docs/`
contains references and `scripts/` contains firmware utilities and explicit
mainframe synchronization.

```
<project>/    (channel_card/ or effect_card/)
├── CMakeLists.txt            ← hand-written sources + TinyUSB go HERE
├── CMakePresets.json         ← Debug / Release presets
├── <project>.ioc             ← STM32CubeMX project (source of truth)
├── *.ld                      ← linker script
├── Core/
│   ├── Inc/  Src/            ← generated init + hand-written drivers
├── cmake/
│   ├── gcc-arm-none-eabi.cmake
│   └── stm32cubemx/          ← GENERATED — never edit
├── Drivers/                  ← STM32 HAL + CMSIS (vendored)
├── ThirdParty/tinyusb*/      ← TinyUSB (vendored, CubeMX-safe location)
├── USB_APP/                  ← USB descriptors, tusb_config.h, class glue
└── build/Debug/              ← build output (.elf/.hex/.bin)
```

Effect Card retains TinyUSB 0.18 for its isochronous microphone fixes.
Channel Card has no TinyUSB dependency or audio USB interface.

---

## 6. Consoles — how to talk to the boards

Both cards run line-based RS485 consoles. Effect Card also has a USB CDC
console; Channel Card USB is binary-only for BODY and uploads. The complete live
command references are in the
[`Channel Card README`](channel_card/README.md#console-command-reference) and
[`Effect Card README`](effect_card/README.md#console-command-reference).
[`docs/protocol.md`](docs/protocol.md) defines framing, binary uploads, and
reply formats.

**Channel Card** — 8 SAMPLE voices (`n0`…`n7`) with automatically allocated,
script-controlled oscillators using eight reserved logical wavetables
(`osc0`…`osc7` in the UI) and ABI2 per-note audio/FM/AM routing, sample upload/assignment
(`al`/`wl`/`ar`/`aw`/`a`/`vq`), per-voice VM programs (`vmload`/`vm`) and LPF
(`f`/`fk`) and DAC gain (`g`).

| Command | Meaning |
| ------- | ------- |
| `n0`…`n7 on <key> [@session]` | Start one scripted voice with MIDI key 0…127. |
| `n0`…`n7 off` / `n off` | Release one voice or all voices. |
| `g <ch> <dB>` | Set CS4304 attenuation on channel 1…4 to 0…127 dB. |

Example: `c:n0 on 69`, followed by `c:n0 off`.

Addressing on the shared RS485 bus: `c:` / `e:` / `*:`. The Effect Card
console covers 48 V, ADC registers, USB channel selection, and LEDs.

See [`docs/protocol.md`](docs/protocol.md) (host↔card protocol) and
[`docs/reference/rs485_console_architecture.md`](docs/reference/rs485_console_architecture.md)
(bus framing).
Per-voice digital LPF: [`docs/reference/note_filter_butterworth.md`](docs/reference/note_filter_butterworth.md).

**USB CDC** — Channel Card binary transport on `/dev/cu.usbmodem*` or
`/dev/ttyACM*`. Use RS485 for Channel text commands. Effect Card CDC runs
its own console (`fw console effect`).

**Playback application** — install system dependencies and build from
[`voice_bd/`](voice_bd/README.md):

```bash
cd voice_bd
make
./voicebd piano.wav
```

Configure device ports in `voicebd.h` for the target machine. To explicitly
synchronize the implementation into a mainframe checkout:

```bash
scripts/sync_voicebd.sh /path/to/175-mainframe
```

Run the sync command from the repository root. It copies only `voicebd.cpp`,
prints differences, and backs up the previous destination. It never changes
`voicebd.h` or commits to SVN.

---

## 7. USB descriptor changes — bump the PID

Windows caches USB descriptors **per VID/PID**. If you change a
descriptor (endpoint size, interfaces, controls) and keep the same PID,
Windows may serve stale cached data and the device misbehaves in ways
that look like firmware bugs; this failure mode has produced multi-hour
misdiagnoses on this project.

**Rule: change a descriptor → bump `idProduct` in
`USB_APP/usb_descriptors.c`.** Current values are `0xCafe/0x401x`
(development identifiers — replace them with organization-owned or formally
allocated VID/PIDs before production).

If a device behaves strangely after a firmware change: Device Manager →
**View → Show hidden devices** → uninstall the stale entries → replug.

---

## 8. Transport validation status

Channel Card now uses one binary CDC connection for eight SAMPLE voices and
uploads. It retains the 48 kHz signed-int8 source format, 4080-sample rings,
script-controlled playback, DAC/CV output and RS485 control/status. Its custom
USB device implementation replaces TinyUSB; the host no longer uses RtAudio.

Debug/Release builds and native protocol/scheduler tests are available. Physical
USB enumeration, throughput and playback latency require qualification on the
board; see [the validation procedure](channel_card/docs/cdc_validation.md).

Effect Card remains the existing mono 32-bit 96 kHz UAC2 microphone with CDC
console, eight ADC inputs and 48 V rail control.
