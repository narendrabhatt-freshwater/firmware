# Freshwater CMI — Firmware and Host Tools

Common quick-start for **both** cards. Per-card detail lives in each
project's own `README.md`; the host↔card wire contract lives in
[`docs/protocol.md`](docs/protocol.md).

| Card         | Folder               | MCU         | CMake target  | CubeMX file       |
| ------------ | -------------------- | ----------- | ------------- | ----------------- |
| Channel Card | `channel_card/` | STM32H725xG | `channel_MCU` | `channel_MCU.ioc` |
| Effect Card  | `effect_card/`  | STM32H743xx | `effect_card` | `effect_card.ioc` |

Both are standalone CMake projects — build them independently. Each is
self-contained: all dependencies (HAL, CMSIS, TinyUSB) are vendored
in-tree, so a copy-paste of either `channel_card/` or
`effect_card/` folder builds as-is with no package manager or
submodule fetch. The C++17 `cardlink` host SDK and its `cmi_control` example
application are separate from the two firmware projects — see §6.

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

CubeMX manages and prunes `Middlewares/`. TinyUSB was deleted from there
twice by regenerations, so it now lives in **`ThirdParty/`**, which
CubeMX does not touch. Keep it that way.

### 4.4 Do not enable the ST USB Device middleware

USB is owned by **TinyUSB** on both cards. In CubeMX:

- Connectivity → **USB_OTG_xS: Device_Only**
- NVIC → **OTG global interrupt: enabled**
- Middleware → **USB_DEVICE class: Disable**

Enabling ST's USB Device class pulls in a second stack that fights
TinyUSB for the same peripheral. The generated
`MX_USB_OTG_xS_PCD_Init()` is harmless and expected — TinyUSB re-owns
the core registers in `USB_App_Init()`, and `HAL_PCD_Start()` is never
called. The USB interrupt is routed to `tud_int_handler()` from a USER
CODE block in `stm32h7xx_it.c`.

### 4.5 Post-regeneration checklist

After every regeneration, verify these survived (they are all in USER
CODE blocks, but check anyway):

- [ ] `USB_App_Init()` called in `main()`
- [ ] `USB_App_Task()` called in the main `while(1)` loop
- [ ] `tud_int_handler(0)` in the OTG IRQ handler in `stm32h7xx_it.c`
- [ ] Hand-written sources still listed in the **top-level** `CMakeLists.txt`
- [ ] Project builds and the board still enumerates over USB

---

## 5. Repository layout (same shape in both projects)

The firmware projects live at the repo root (`channel_card/`,
`effect_card/`), alongside the C++17 `cardlink` host SDK under
`protocol/` and the `cmi_control` example application.
The `fw` CLI wrapping all of this lives in `scripts/` at the repo root.

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

**TinyUSB versions differ per card** (deliberately):

- Effect Card → `ThirdParty/tinyusb-0.18`
- Channel Card → `ThirdParty/tinyusb` (0.17)

The Effect Card was upgraded for dwc2 isochronous-IN fixes. Do not
unify the versions without re-testing audio on both cards. Both vendor
trees are pruned to `src/` + license; a 0.17 rollback for the Effect
Card is recoverable from version-control history.

---

## 6. Consoles — how to talk to the boards

Both cards run the same line-based console over RS485 and USB CDC.
[`docs/protocol.md`](docs/protocol.md) is the normative command
reference; the summary:

**Channel Card** — 8 SAMPLE voices (`n0`…`n7`, summed on CH1), fallback
oscillator shape (`s`/`p`/`t`), sample upload/assignment
(`al`/`ar`/`aw`/`a`/`vq`), per-voice envelope (`en`/`ek`) and LPF
(`f`/`fk`), DAC gain (`g`), CPU probe (`cpu`).

| Command                        | Meaning                                                         |
| ------------------------------ | --------------------------------------------------------------- |
| `n0`                           | Session defaults: **bypass on** + **g 1 0**                     |
| `n0`…`n7` `0` / `<Hz> [scale]` | Note off/on; optional scale 0..1 (default 0.125); summed on CH1 |
| `g <ch> <dB>`                  | CS4304 DAC atten on CH1..4, dB 0..127                           |

Examples: `n0 440 0.5`, `n1 550` (scale 0.125), `n2 660 0.1`.

Addressing on the shared RS485 bus: `c:` / `e:` / `*:`. The Effect Card
console covers 48 V, ADC registers, USB channel select and LEDs — see
`docs/protocol.md` §3.

See [`docs/protocol.md`](docs/protocol.md) (host↔card protocol) and
[`docs/reference/rs485_console_architecture.md`](docs/reference/rs485_console_architecture.md)
(bus framing).
Per-voice digital LPF: [`docs/reference/note_filter_butterworth.md`](docs/reference/note_filter_butterworth.md).

**USB CDC** — same Channel Card parser on `/dev/cu.usbmodem*`.
Effect Card CDC runs its own console (`fw console effect`).

**Control GUI** (the supported cardlink example; Dear ImGui + GLFW — MIDI +
console + preview scope; macOS / Linux / Windows) —

```bash
fw control build
fw control
```

See [`cmi_control/README.md`](cmi_control/README.md).

---

## 7. USB descriptor changes — bump the PID

Windows caches USB descriptors **per VID/PID**. If you change a
descriptor (endpoint size, interfaces, controls) and keep the same PID,
Windows may serve stale cached data and the device misbehaves in ways
that look like firmware bugs; this failure mode has produced multi-hour
misdiagnoses on this project.

**Rule: change a descriptor → bump `idProduct` in
`USB_APP/usb_descriptors.c`.** Current values are `0xCafe/0x401x`
(placeholders — replace with a real VID/PID before production).

If a device behaves strangely after a firmware change: Device Manager →
**View → Show hidden devices** → uninstall the stale entries → replug.

---

## 8. Known-good baseline

Both cards currently build clean (no warnings) and are working on
hardware:

- **Channel Card** — 8 SAMPLE voices: vendor bulk BODY (packed int16
  bursts per voice) feeds per-voice sustain; rate-scaled attack heads
  uploaded over CDC; CS4304 DAC out; DC control voltages on CH2–CH4 (0 V
  at boot); RS485 + USB CDC consoles.
- **Effect Card** — 8-channel capture from two TLV320ADC6140 ADCs over
  TDM/SAI, one selectable channel streamed to the PC (UAC2 microphone,
  mono 32-bit 96 kHz), 48 V phantom rail control, RS485 + USB CDC
  consoles.
- **`cmi_control`** — Dear ImGui + GLFW control surface (MIDI, RS485
  console, local preview scope) and the supported SDK example. See
  [`cmi_control/README.md`](cmi_control/README.md).
- **`protocol`** — Single C++17 host SDK: `cardproto` wire API,
  shared serial, RS485 tagged bus / `Bus`, USB CDC and sample upload, MIDI
  input/voice allocation, local speaker output, and vendor bulk BODY. See
  [`protocol/README.md`](protocol/README.md).
