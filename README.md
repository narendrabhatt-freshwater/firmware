# Freshwater CMI — Firmware Handover Manual

Common quick-start for **both** cards. Per-card detail lives in each
project's own `README.md` (to be expanded).

| Card | Folder | MCU | CMake target | CubeMX file |
|---|---|---|---|---|
| Channel Card | `channel_card/` | STM32H725xG | `channel_MCU` | `channel_MCU.ioc` |
| Effect Card  | `effect_card/`  | STM32H743xx | `effect_card` | `effect_card.ioc` |

Both are standalone CMake projects — build them independently. The whole
`firmware/` folder is self-contained: all dependencies (HAL, CMSIS,
TinyUSB) are vendored in-tree, so a copy-paste of this folder builds
as-is with no package manager or submodule fetch.

---

## 1. Tools to install

| Tool | Why | Notes |
|---|---|---|
| **STM32CubeCLT** | CMake + Ninja + `arm-none-eabi-gcc` toolchain | Provides the whole build chain. Easiest single install. |
| **STM32CubeMX** | Regenerating the HAL/peripheral framework from the `.ioc` | Only needed if you change pinout/peripherals |
| **STM32CubeProgrammer** | Flashing over USB DFU | Also installs the DFU USB driver |

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
done for e.g. `Core/Src/cs4304.c` and `Core/Src/audio_bridge.c` on the
Channel Card — both were dropped by a regeneration once, which is why
they now live in the top-level file.

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

```
<project>/
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

- Effect Card → `ThirdParty/tinyusb-0.18` (0.17 kept alongside as a
  rollback; switch by editing the paths in `CMakeLists.txt`)
- Channel Card → `ThirdParty/tinyusb` (0.17)

The Effect Card was upgraded for dwc2 isochronous-IN fixes. Do not
"tidy up" by forcing both to one version without re-testing audio.

---

## 6. Consoles — how to talk to the boards

Both cards expose the **same command console** over two transports.

**RS485 multi-drop** (both cards share one bus, 115200 8N1):

| Prefix | Goes to |
|---|---|
| `c:` | Channel Card only |
| `e:` | Effect Card only |
| `*:` or no prefix | Both |

```
c:help
e:status
```

**USB CDC** — each card also enumerates a virtual COM port running the
same parser. Open it at any baud rate; commands there are addressed to
that card (a bare `help` works).

Type `help` on either transport for the current command list.

---

## 7. USB descriptor changes — bump the PID

Windows caches USB descriptors **per VID/PID**. If you change a
descriptor (endpoint size, interfaces, controls) and keep the same PID,
Windows may serve stale cached data and the device misbehaves in ways
that look like firmware bugs — this cost a full debugging session once.

**Rule: change a descriptor → bump `idProduct` in
`USB_APP/usb_descriptors.c`.** Current values are `0xCafe/0x401x`
(placeholders — replace with a real VID/PID before production).

If a device behaves strangely after a firmware change: Device Manager →
**View → Show hidden devices** → uninstall the stale entries → replug.

---

## 8. Known-good baseline

Both cards currently build clean (no warnings) and are working on
hardware:

- **Channel Card** — USB audio playback (UAC2 speaker, mono 32-bit
  96 kHz) → CS4304 DAC, plus tone/DC generators on CH2–CH4, RS485 +
  USB CDC consoles.
- **Effect Card** — 8-channel capture from two TLV320ADC6140 ADCs over
  TDM/SAI, one selectable channel streamed to the PC (UAC2 microphone,
  mono 32-bit 96 kHz), 48 V phantom rail control, RS485 + USB CDC
  consoles.

> **Not under version control yet.** Initialising a git repo at
> `firmware/` and committing this baseline is strongly recommended
> before further work — several issues in development were caused by
> regenerations silently removing files, which a `git diff` would have
> caught immediately.
