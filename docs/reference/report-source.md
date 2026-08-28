# Channel-card uploaded control programs: research source

Audience: Freshwater firmware team

Date: 2026-08-28
Decision: Whether a user-supplied program can execute on the Channel Card and generate 8 voices x 3, potentially 10, control parameters at 1 kHz while BODY audio streaming continues.

## Scope and assumptions

- "Generate" means compute parameter setpoints locally, not emit 24 or 80 physical analogue outputs.
- The initial rate is 8 x 3 x 1,000 = 24,000 float values/s. The future rate is 8 x 10 x 1,000 = 80,000 values/s, not 240,000.
- The program is uploaded infrequently and runs every millisecond. Native firmware continues sample-rate audio, interpolation, filtering, clamping, and hardware access.
- Existing USB UAC BODY streaming and RS-485 commands must continue without underrun or excessive latency.
- Scripts may eventually be user-authored and faulty. Whether they are adversarial is a product-policy question; the recommended design is safe for faulty programs and can be strengthened for untrusted programs.

## Direct answer

Yes, this is feasible on the STM32H725, but not by streaming float32 setpoints over the existing RS-485 link or appending them to the existing UAC packet. Upload a compact program once, execute it locally at control rate, and double-buffer its outputs. The card has abundant CPU and flash for this rate, but very little currently unallocated RAM and a hard 1 ms audio cadence. Consequently, use a fixed-memory, budgeted runtime and never execute the program in the I2S DMA callback.

The recommended production design is a small purpose-built control VM with bounded loops, fixed state, no allocation during `tick`, an instruction/cycle budget, and a narrow parameter API. Berry is the most credible quick scripting prototype. WAMR is the most credible general compiled/sandboxed experiment if C/Rust/AssemblyScript authoring is acceptable. Lua can work for trusted development scripts but is not the best production default here. MicroPython is a poor memory/timing fit, and Cython is not a standalone MCU runtime.

## Repository evidence

- Target: STM32H725xG, 550 MHz Cortex-M7 configuration, 1 MiB flash, internal SRAM banks, hard-float FPv5-D16, Release `-O3`.
- Audio: 48 kHz, 48-frame/1 ms DMA half-buffer. The I2S callback currently renders all active voices and per-sample envelopes/filters.
- Existing programmed envelopes can call `NoteEnv_Process` once for every active voice sample, up to 8 x 48,000 = 384,000 float envelope results/s. This is 16 times the initial requested result count, although a general VM operation is more expensive than this small native state machine.
- Transport: RS-485 is 921600 8N1. UAC is 10 channels x int16 x 51 frames/ms = 1,020 B/ms and carries one tag plus 509 BODY samples.
- Release map snapshot: flash load image ends near 115,344 B (~933,232 B nominally unused); DTCM has about 15,064 B unassigned after data/BSS/reserved heap+stack; AXI RAM has about 27,432 B unassigned after DMA buffers and the 256 KiB attack bank. D2/D3/ITCM are effectively consumed by BODY rings. These are linker occupancy figures, not proven safe heap capacity.
- Physical-output caveat: DAC channels 2-4 are three channel-global CVs after the eight voices are mixed to DAC channel 1. Eight voices x N can be internal DSP parameters, but the current hardware cannot expose 24 independent analogue CVs.

## Quantification

### Transport

At 8N1, the absolute RS-485 byte rate is 921600 / 10 = 92,160 B/s:

- Initial float32: 24,000 x 4 = 96,000 B/s before any addressing, framing, CRC, or replies. Impossible.
- Future float32: 80,000 x 4 = 320,000 B/s. Impossible.
- Initial Q15/float16: 48,000 B/s. Possible in raw bandwidth but consumes over half the bus and adds serialization/jitter.
- Future Q15/float16: 160,000 B/s. Impossible.

The UAC payload is 1,020 B every 1 ms. A full-speed isochronous endpoint's maximum packet is 1,023 B, so only three bytes remain in that endpoint packet. A separate endpoint/CDC design could carry controls, but it would compete with a deliberately dense BODY transport and retain host/link jitter. The uploaded-program design removes the recurring control bandwidth.

### Compute

At 550 MHz there are 550,000 gross core cycles per 1 ms tick. The output blocks are only 96 B initially or 320 B at ten parameters; double buffering costs 192 B or 640 B. The number of outputs is therefore not the compute problem. Algorithm complexity, runtime dispatch cost, garbage collection, and worst-case execution are the real variables.

An engineering acceptance target, to be proven on the board rather than assumed, is a <=50-100 us maximum runtime for an allowed program while all eight voices, filters, USB BODY, and console traffic are active. That gives the script 27,500-55,000 cycles/tick and leaves most of the millisecond for audio and transport. Calibrate the VM's operation budget from measured worst-case cycles, not an optimistic desktop benchmark.

### Signal quality

A 1 kHz control rate has a 1 ms granularity and cannot represent modulation above 500 Hz. It is appropriate for envelopes, LFOs, curves, sequencers, and control coefficients if native code linearly interpolates or slews across the next 48 audio frames. Audio-rate FM or arbitrary audio DSP should remain native at 48 kHz or use a separately assessed DSP/AOT path.

### Live manipulation requirement

The uploaded program must publish a manifest of live controls: stable IDs/names, types, ranges, defaults, smoothing policy, and global/per-voice scope. The host sends sparse changes such as LFO rate/depth/shape, phase-reset triggers, envelope times, or modulation destinations; the card continues generating the time-varying LFO/envelope output locally.

Updates enter a fixed-size event ring as `(program version, control ID, value, sequence, optional apply tick/ramp)`. The control runner drains a bounded number at the next tick and gives the VM one immutable snapshot, preventing mid-tick tearing. Floats are clamped/smoothed natively; gates, retriggers, note events, and phase resets are queued events rather than levels so short pulses are not lost. Stale program versions and queue overflow fail safely and are counted.

This traffic is sparse and therefore compatible with CDC or RS-485 at normal UI/knob update rates. A sub-audio LFO runs well at the 1 kHz script rate with native 48-sample interpolation. Faster modulation should use a native audio-rate oscillator whose controls are manipulated by the program.

## Runtime candidates

### Purpose-built bounded control VM — recommended

Use a host compiler with a Lua-like or expression syntax, but emit a small verified bytecode specific to control generation. Allow float32 arithmetic, constants, fixed state registers, comparisons, select, clamp, bounded loops, and selected native intrinsics. Disallow recursion, dynamic allocation, strings/tables in `tick`, arbitrary pointer access, driver calls, and unbounded loops. Validate all branches and maximum operations before activation. This yields deterministic time and memory and keeps the ABI narrow.

### Berry — best fast prototype

Berry is an ANSI C99 register VM designed for microcontrollers. Its project reports under 40 KiB interpreter-core text and operation with under 4 KiB heap on Cortex-M4; it supports bytecode files and mark-sweep GC. It also has an observability hook used to interrupt runaway programs. It is a substantially better memory fit than Python-class runtimes, but the claimed footprint is not this exact configuration, GC remains a timing risk, and the hook cadence must be tightened and measured. Use protected calls, preallocated objects, no allocation during `tick`, a fixed allocator arena, and trusted development scripts for the first experiment.

### WAMR / WebAssembly — strongest general sandbox experiment

WAMR supports Cortex-M7, classic/fast interpreters and AOT. Its project reports roughly 56.3 KiB classic-interpreter text, 58.9 KiB fast-interpreter text, or 29.4 KiB AOT runtime text in a Cortex-M4F configuration. WAMR can use a caller-supplied fixed memory pool, and simple no-growth modules can have linear memory below 64 KiB. WebAssembly validation and linear-memory bounds provide a stronger isolation model than native modules.

The caveats are important here: the current free RAM is far below the peak memory published for general CoreMark workloads, although a tiny no-libc control module can be much smaller; WAMR's fast interpreter creates internal bytecode; and AOT files are trusted by default unless the optional AOT validator is enabled. AOT version compatibility also needs exact compiler/runtime pinning. Start with validated Wasm bytecode or enable the AOT validator; do not use the mini-loader or disabled bounds checks. A hard per-tick instruction budget may require integration work beyond WAMR's termination APIs.

### Lua — viable trusted mode, not the default

Lua is a compact embeddable register VM and `luac` emits precompiled chunks. It supports a custom allocator, incremental/generational GC, count hooks, and a 32-bit integer/float configuration. However, precompiled chunks are version-specific, standard Lua bytecode is not a stable interchange ABI, Lua warns that executing maliciously constructed binary chunks can crash the interpreter, and GC/dynamic tables complicate hard deadlines. It can be made workable with a fixed arena, a stripped standard library, 32-bit numbers, count hooks, no tick-time allocation, protected calls, and only trusted compiler output. Berry reaches the first prototype with a tighter advertised embedded footprint.

### MicroPython — not recommended here

MicroPython can cross-compile `.mpy` bytecode and can load ARMv7E-M native modules. Its own guidance notes heap allocation/fragmentation concerns, that some ports allocate floats on the heap, and that Viper floating point is not optimized. Its runtime and language surface are unnecessary for a small deterministic controller, especially with only ~15 KiB unassigned DTCM.

### Cython and direct native code

Cython generates C/C++ compiled as extension modules that execute in the CPython runtime; it is not a drop-in bare-metal scripting solution. A custom C-to-native upload or MicroPython native `.mpy` would be fast, but uploaded native pointers and code can corrupt firmware, and relocation/version/signing/MPU design becomes the project. Reserve native modules for signed, trusted vendor code after the control API is stable.

## Recommended architecture

1. Author a control program on the host. Compile and statically validate it on the host; send bytecode, ABI version, live-control manifest, limits, length, CRC, and optionally a signature over USB CDC. Do not send source or recurring controls through UAC.
2. Receive into an inactive bounded slot. Revalidate length, bytecode structure, state/output bounds, loop bounds, imported functions, and CRC on the card. CRC detects corruption; signatures/authentication are needed if hostile uploads are in scope.
3. Instantiate only from a fixed arena. Run `init` and several dry ticks before atomically selecting it. Keep the previous known-good slot.
4. The 1 ms DMA callback only timestamps/signals a control tick and consumes the last completed parameter block. It never calls the VM.
5. Sparse live-control writes enter a bounded mailbox. A main-loop or low-priority control runner drains a bounded number, snapshots immutable controls and musical inputs, and evaluates `tick(dt, inputs, outputs)` into the inactive 8 x P float32 block under a measured cycle/operation budget.
6. Validate every output for finite value and range, apply native clamps/slew/interpolation, then atomically publish. If late or faulted, hold the prior values or crossfade to safe defaults. Disable after repeated overruns.
7. Native 48 kHz code applies interpolated parameters to each voice. Hardware and transport APIs are not exposed to the program.
8. Export counters: current/max cycles, operation count, late ticks, VM faults, invalid outputs, active program hash/version, arena high-water mark, and audio `fill_late`/USB drop/hold counters.

## Proof-of-concept and acceptance gate

Stage A establishes the schedule without a VM: write the worst expected control algorithm in C, run it in the proposed low-priority double-buffer path, and measure DWT cycles under eight voices/eight filters, sustained BODY, CDC and RS-485 traffic.

Stage B implements a RAM-only uploaded bounded VM with about a dozen operations, fixed state, compile-time-bounded loops, CRC/version validation, and a hard budget. Do not add persistent flash or a rich language yet.

Stage B also implements smoothed float, enum/integer, gate, and one-shot trigger controls. It must demonstrate live LFO rate/depth changes and phase reset without recompiling or re-uploading the program.

Stage C benchmarks the same three kernels in the custom VM and Berry; add WAMR only if multi-language sandboxing is a real requirement. Suggested kernels: eight ADSR/curve state machines; eight LFO + envelope + clamp chains; and a worst-allowed branch/loop program.

Go/no-go criteria:

- All allowed programs provably terminate within an enforced budget.
- Measured maximum control runtime <=100 us (initial target) under the full integrated load.
- Zero I2S fill-late events and no increase in USB BODY drop/hold counters during a sustained soak.
- Fixed arena fits without reducing the reserved 16 KiB stack or silently colliding with current DTCM/AXI allocations.
- Invalid, infinite-loop, NaN/Inf, oversized, truncated, bad-CRC, and wrong-version programs fail safely while audio continues.
- A missed tick holds/crossfades safely; upload/activation never blocks the audio ISR.

## Material limitations

- No board CPU-duty measurement was available in this research. Feasibility is high based on rate, existing native workload and clock, but the integrated worst-case runtime remains an empirical gate.
- The release map may not exactly represent the current dirty worktree. Memory figures are a planning snapshot and must be regenerated for the prototype build.
- Berry and WAMR footprint numbers are upstream configurations, not this firmware build. Only a stripped integration map establishes fit.
- "Reasonable algorithms" needs a product definition. The recommendation covers control-rate state machines and math, not arbitrary audio-rate DSP or general-purpose programs.

## Claim-to-source ledger

- MCU capability: STMicroelectronics, "STM32H725IG," current product page; 550 MHz Cortex-M7, DP-FPU, 1 MiB flash, 564 KiB RAM. https://www.st.com/content/st_com/en/products/microcontrollers-microprocessors/stm32-32-bit-arm-cortex-mcus/stm32-high-performance-mcus/stm32h7-series/stm32h725-735/stm32h725ig.html
- Repository configuration and architecture: `channel_card/README.md`, `channel_card/channel_MCU.ioc`, `channel_card/STM32H725xG_flash.ld`, `channel_card/build/Release/channel_MCU.map`, `channel_card/cmake/gcc-arm-none-eabi.cmake`, `channel_card/Core/Src/audio/audio_bridge.c`, `channel_card/Core/Src/audio/note_bank.c`, `channel_card/Core/Src/audio/note_envelope.c`, `channel_card/USB_APP/usb_stream.h`, `channel_card/docs/protocol.md`; accessed 2026-08-28.
- USB full-speed isochronous packet maximum: USB-IF, USB 2.0 specification, §5.6.3/endpoint descriptor rules; upstream specification family at https://www.usb.org/document-library/usb-20-specification
- Lua VM, GC, binary chunk warning, number configuration and APIs: PUC-Rio, "Lua 5.4 Reference Manual," updated through 2025/2026. https://www.lua.org/manual/5.4/manual.html
- Lua VM bytecode version incompatibility: PUC-Rio, "Lua: version history." https://www.lua.org/versions.html
- Berry footprint, bytecode and GC: berry-lang, project README. https://github.com/berry-lang/berry
- Berry solidification/bytecode behavior: Berry documentation, "Advanced features." https://berry.readthedocs.io/en/latest/source/en/Chapter-8.html
- Berry runaway-program hook: berry-lang discussion #430, maintainer response, 2024-06-13. https://github.com/berry-lang/berry/discussions/430
- WAMR modes, footprint and Cortex-M7 support: Bytecode Alliance, WAMR project README. https://github.com/bytecodealliance/wasm-micro-runtime
- WAMR fixed pools and memory categories: Bytecode Alliance, "The WAMR memory model," 2023-03-21. https://bytecodealliance.github.io/wamr.dev/blog/the-wamr-memory-model/
- WAMR sub-64 KiB linear-memory tuning: Bytecode Alliance, "Understand the WAMR heaps," 2023-03-17. https://bytecodealliance.github.io/wamr.dev/blog/understand-the-wamr-heaps/
- WAMR validation/security: Bytecode Alliance, "WAMR Security Features." https://github.com/wasm-micro-runtime/wasm-micro-runtime/blob/main/gitbook/basics/introduction/security_feature.md
- WAMR AOT validator and mini-loader caveats: Bytecode Alliance, `doc/build_wamr.md`. https://github.com/bytecodealliance/wasm-micro-runtime/blob/main/doc/build_wamr.md
- WAMR AOT compatibility: Bytecode Alliance, `doc/build_wasm_app.md`. https://github.com/bytecodealliance/wasm-micro-runtime/blob/main/doc/build_wasm_app.md
- WAMR benchmark limitations: Bytecode Alliance, "Performance," 2020 dataset. https://github.com/bytecodealliance/wasm-micro-runtime/wiki/Performance
- WebAssembly sandbox semantics: WebAssembly Community Group, "Security." https://webassembly.org/docs/security/
- MicroPython bytecode/native modules and timing/memory concerns: MicroPython project documentation. https://docs.micropython.org/en/latest/develop/optimizations.html ; https://docs.micropython.org/en/latest/develop/natmod.html ; https://docs.micropython.org/en/latest/reference/speed_python.html
- Cython runtime category: Cython project, "Cython — an overview." https://docs.cython.org/en/latest/src/quickstart/overview.html

## Search record and stop rationale

Searched official STM32 documentation, repository implementation/build artifacts, Lua and MicroPython manuals, Berry source documentation, Bytecode Alliance WAMR documentation/source, WebAssembly security documentation, USB-IF specifications, and Cython documentation. Follow-up searches covered runtime memory, bytecode validation/versioning, AOT security, runaway-program termination, and Cortex-M7 evidence. Research stopped because transport feasibility, compute/memory constraints, candidate capabilities, and safety caveats all have direct evidence; remaining uncertainty is implementation-specific board benchmarking, which further web retrieval cannot resolve.
