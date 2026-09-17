# USB CDC migration audit — 17 September 2026

Compared the immediate pre-migration commit `92f879b` with migration commit
`58371a3`. Both were exported into `/tmp/freshwater-usb-audit/` and built clean
with the same Arm GNU Toolchain 15.3.Rel1, default 921600-baud configuration,
and the checked-in CMake toolchain. Release and Debug were compared separately.
No device was flashed or playback started during this audit.

## Voices and USB scheduling

The new protocol carries **one voice per BODY message, not one voice per
millisecond**. The card still has eight independent voice rings and mixes all
active voices into each output sample.

| Property | Previous UAC transport | Current CDC transport |
| --- | --- | --- |
| USB device interfaces | Audio streaming plus CDC | One CDC ACM serial function |
| BODY transport | Fixed 1,008-byte ISO payload each 1 ms while streaming | Variable-length bulk byte stream |
| Application framing | 10-byte header, up to two voice blocks, 998 total sample bytes | 8-byte header per block, one voice, 1–1,024 sample bytes |
| Physical USB packets | Full-speed isochronous | Full-speed bulk, 64-byte packets |
| Voice selection | Up to two voices per ISO packet | Repeated priority/fairness selection across all eight voices |
| Host batching | Audio callback packs fixed packets | Multiple messages per write, up to 8,256 outstanding BODY wire bytes |
| Status/credits/note controls | RS485 | RS485, normally every 5 ms for status |
| No active voices | ISO padding while audio stream is open | No BODY data; RS485 polls and USB housekeeping can remain |

`voice_bd/voicebd.cpp:make_block()` examines all voices, prioritizes near
deadlines and initial priming, accounts for outstanding data, and rotates its
starting voice. `usb_link::run()` calls it repeatedly to concatenate blocks
before a write. A full-sized BODY message spans multiple 64-byte USB packets;
message boundaries are not USB packet boundaries. Neither a message nor a
serial write receives a dedicated 1 ms slot.

The firmware parses each block and routes it by voice and session ID to its
ring. `NoteBank_NextSample()` continues to sum all active voices. Capacity is
still **4,080 signed 8-bit samples per voice**; initial BODY priming is still
**998 samples**. Nominal eight-voice demand remains 384,000 sample bytes/s,
or 768,000 at the existing maximum 2x consumption rate, plus headers.

The GUI selects one instrument for an eight-note polyphonic keyboard. Its six
octave sample banks preserve upper-key pitch; they do not reduce voice count
or create extra USB ports. The GUI and its decoder are host code only.

## Exact binary and memory comparison

| Matched build / allocation | Before | After | Change |
| --- | ---: | ---: | ---: |
| Release `.bin` | 172,320 B | 167,372 B | **−4,948 B (−2.87%)** |
| Debug `.bin` | 231,500 B | 219,700 B | **−11,800 B** |
| Release `.text` | 150,840 B | 145,712 B | −5,128 B |
| Release `.rodata` | 20,048 B | 20,272 B | +224 B |
| Release initialized `.data` | 560 B | 516 B | −44 B |
| Release DTCM region used, including reserved heap/stack | 66,176 B | 75,184 B | +9,008 B |
| Release AXI SRAM region used | 269,664 B | 233,488 B | −36,176 B |
| Combined reserved RAM in those regions | 335,840 B | 308,672 B | **−27,168 B** |

`.bin` includes loadable code/constants and the initialization image for
`.data`. The NOLOAD voice rings, USB queues, VM arena, attack bank and reserved
stack do not enlarge it. `size`'s `dec` total includes BSS and is not the flash
binary size. Region usage includes alignment and reservations; it is not a
measurement of worst-case runtime stack use.

Existing local files independently show the same direction:

- `channel_card/build/Release/channel_MCU.bin`: **172,328 B**, pre-migration
  artifact dated 14 September.
- `channel_card/build/cdc/Release/channel_MCU.bin`: **167,372 B**, new
  Release artifact dated 17 September.
- `/tmp/freshwater-cdc-debug/channel_MCU.bin`: **219,708 B**, a separate new
  Debug artifact. Comparing it with the old Release artifact gives an apparent
  increase of 47,380 B, but mixes build types and is not a migration comparison.

The precise pair the user compared was not identified. Both clean like-for-like
comparisons are smaller. The toolchain uses `-O3 -g0` for Release and `-O0 -g3`
for Debug. Debug's unoptimized machine code can increase `.bin` size; ELF debug
symbols themselves are not copied into `.bin`. `scripts/fw` already defaulted
to Debug before this migration. Use `scripts/fw build channel --release` for
the performance build; this audit does not change that default.

## Why deleting TinyUSB saves only about 5 KB of flash

The linker previously retained only part of the vendored TinyUSB tree. Removing
approximately 79,000 source lines is not removing that many lines' worth of
machine code: most classes/controllers were never compiled or were discarded
by `--gc-sections`.

Live input-section totals from the Release linker maps, including code,
constants and initialized data (alignment is accounted separately):

| Linked component | Before | After | Change |
| --- | ---: | ---: | ---: |
| TinyUSB objects | 15,761 B | 0 B | −15,761 B |
| Custom USB application/device/descriptors | 2,030 B | 6,680 B | +4,650 B |
| STM32 HAL PCD + PCD extension | 1,088 B | 4,792 B | +3,704 B |
| STM32 low-level USB driver | 1,184 B | 3,444 B | +2,260 B |
| Those USB components combined | 20,063 B | 14,916 B | **−5,147 B** |

The new stack is small custom CDC logic over STM32 HAL, not a register-only
implementation with no libraries. TinyUSB previously owned the USB interrupt;
the new code calls HAL PCD, retaining HAL interrupt/endpoint routines that the
old link could discard. Other small changes and alignment bring the net whole
binary saving to 4,948 B. Diagnostics, framing/validation, reconnect handling
and the USB probe are part of the new USB footprint.

Substantial unchanged flash users include the Berry runtime/backend objects
(44,707 B), linked newlib-nano objects (21,895 B), and linked math objects
(14,232 B). Float console support, synthesis, filtering and the DAC drivers
were not removed. These are linked-section contributions, not estimates based
on source sizes.

RAM fell because the old USB/audio buffers occupied 36,172 bytes of the
`.dma_buffer` section. The remaining `.dma_buffer` is the unchanged 1,536-byte
I2S allocation. The new CDC RX queue (8,192 B), TX queue (2,048 B), endpoint
buffers and parser are in fast DTCM. Overall `.bss` grows by 9,040 B while the
combined RAM-region allocation falls by 27,168 B. The 100,880-byte VM arena,
131,072-byte attack bank and 16 KiB stack reservation remain unchanged.

## Change inventory

1. **USB firmware:** removed Channel Card TinyUSB/UAC; added CDC descriptors,
   endpoint/control handling through HAL PCD, RX/TX queues, an 8-byte framed
   protocol, HELLO, BODY, chunked uploads, upload abort/reply, and a diagnostic
   probe. PID changed from `cafe:4031` to `cafe:4032`.
2. **Ring ingestion/concurrency:** replaced the two-block UAC decoder with
   voice/session BODY dispatch; retained a temporarily blocked frame instead
   of overwriting a full ring; made final publication atomic across interrupt
   preemption; handled pending-to-current note promotion during a reserved
   write; made shared session/generation fields volatile.
3. **USB recovery/performance:** corrected SET_ADDRESS sequencing, preserved
   endpoint DATA toggles across DTR reopening, moved queues into DTCM, used
   bounded memcpy operations and short interrupt critical sections, and added
   RS485-accessible USB diagnostics.
4. **RS485 service:** status layout and control route retained; UART TX now
   fills its FIFO from the interrupt while the waiting main loop services USB.
   Added receive-drop/transmit-failure/truncation counters.
5. **Host transport:** removed RtAudio, added one CDC worker for BODY/uploads,
   partial-I/O handling and explicit wakeups, 64-entry / 8,256-byte outstanding
   accounting, session-aware credits, bounded 5 ms demand prediction with a
   2 ms reserve, and macOS scheduling requests for both transport workers.
6. **Build/tests/docs:** removed Channel TinyUSB build sources and definitions;
   added parser/device/ring/host tests and hardware diagnostics. Some protocol
   documentation is mirrored into Effect Card docs; Effect firmware and
   `175-mainframe/mas` were not migrated.
7. **Subsequent GUI work:** `voicebdgui.cpp`, its tests, host build rules and
   README are uncommitted host-side additions at audit time. They do not change
   the firmware image. The user's existing `devices.h` edits were preserved.

The mixer, DAC cadence (1 ms DMA halves), 48 kHz sample clock, eight-voice
capacity, attack size, Berry engine and filtering are retained. Removing USB
Audio middleware does not remove the audio synthesis/output implementation.

## Findings and remaining qualification

- **No guaranteed 1 ms USB delivery or note-to-DAC latency.** Full-speed bulk
  has no isochronous bandwidth reservation. OS scheduling, hubs/other devices,
  RS485 command timing and initial priming remain relevant. The measured
  1.659 ms median probe round trip is not a one-way or audible latency figure.
- **Shared-stream blocking:** `usb_app.c` retains a complete BODY block if its
  target ring lacks space. Later messages for other voices/uploads must wait
  behind it. This is deliberate lossless backpressure, but its worst-case
  effect under mixed rates/retriggering is not bounded by the existing tests.
- **Scheduling request failures are not surfaced:**
  `prioritize_stream_thread()` discards the return values of QoS/policy calls.
  Earlier normal-priority runs had 19–26 ms worker gaps. A refused realtime
  request can therefore leave a materially different scheduling configuration
  without a host warning. Linux does not request the macOS policy and has no
  recorded hardware qualification.
- **Validation is workload-specific:** recorded final eight-voice 2x playback
  ran approximately 232 seconds with no hold/drop/full/bad/late/future or RS485
  error increments. The GUI's short eight-note test also passed. These establish
  functioning polyphony, not zero-error behavior for every host/load. Matched
  old/new physical key-to-DAC timing, multi-card loading and physical
  unplug/suspend coverage remain outstanding; see `cdc_validation.md`.

No protocol, scheduling, buffer size or firmware optimization was changed as
part of this audit. The next improvements should make scheduling-policy success
observable and measure mixed-voice delivery/latency before changing framing or
shrinking safety buffers.

## Reproduction

Audit artifacts and build logs: `/tmp/freshwater-usb-audit/`.
For each exported revision, use its own toolchain file:

```sh
cmake -S SOURCE/channel_card -B BUILD -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=SOURCE/channel_card/cmake/gcc-arm-none-eabi.cmake
cmake --build BUILD --parallel 6
wc -c BUILD/channel_MCU.bin
arm-none-eabi-size -A BUILD/channel_MCU.elf
```

Repeat with `Debug` in a separate directory. The exact section and object
contributions can be checked in `channel_MCU.map`; the audit's live-input-section
extraction is saved in `/tmp/freshwater-usb-audit/analyze.py` and `objects.json`.
