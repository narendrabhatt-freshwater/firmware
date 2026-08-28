# Feasibility of uploaded control programs on the Channel Card

**Decision report — 28 August 2026**

## Verdict

This is feasible, and local program execution is the right architectural direction.

The requested initial rate—8 voices x 3 parameters x 1,000 updates/s—is only **24,000 float results/s**. Even the possible expansion to ten parameters is **80,000 results/s**. The STM32H725 has ample compute and flash for control-rate algorithms. The constraints are instead:

1. the values cannot fit through the existing RS-485 link as float32;
2. the existing USB BODY endpoint packet is already essentially full;
3. the firmware has little unallocated RAM despite the MCU's large total SRAM; and
4. a faulty or slow program must never delay the 1 ms audio refill.

The best production solution is a **small, fixed-memory control-language VM**. Upload its bytecode once over USB CDC, execute it outside the audio interrupt, and double-buffer its outputs. If the team wants to evaluate an existing language first, **Berry** is the most plausible quick prototype. **WAMR/WebAssembly** is the strongest general sandbox option if authoring in C, Rust, or AssemblyScript is acceptable. Lua can work for trusted experiments but is not the best production default; MicroPython is a poor fit, and Cython is not an MCU runtime.

One terminology correction matters: compiling a script to bytecode does **not** offload computation from the Channel Card. The card still performs the computation. It offloads recurring work from the host and, more importantly, removes the recurring parameter traffic from USB/RS-485.

## What “submit a script and run it on the card” means

The normal flow is:

```text
author source on PC
        |
host compiler -> validated bytecode + ABI/version/limits/CRC
        |
upload once over USB CDC
        |
card validates and instantiates it in a fixed memory arena
        ^
live control updates: rate/depth/shape/gate/etc. -> control mailbox
        |
every 1 ms: tick(inputs) -> 8 x parameter-count setpoints
        |
native firmware clamps, slews/interpolates, and applies them at 48 kHz
```

“Bytecode” is a compact set of instructions for a virtual machine, rather than raw ARM machine code. It is easier to validate and contain than native code. “AOT” means compiling ahead of time to code closer to native speed; it is faster but introduces compatibility, relocation, and safety issues.

The existing BODY audio bytes continue exactly as they do now. Program upload belongs on CDC as an infrequent management operation; control output does not belong inside the UAC BODY packet.

## What the current firmware tells us

The card uses an STM32H725 Cortex-M7 configured at **550 MHz**, with hardware floating point and a 1 MiB flash address space. This matches ST's current description of the part as a 550 MHz Cortex-M7 with double-precision FPU and 564 KiB total SRAM ([STMicroelectronics product page](https://www.st.com/content/st_com/en/products/microcontrollers-microprocessors/stm32-32-bit-arm-cortex-mcus/stm32-high-performance-mcus/stm32h7-series/stm32h725-735/stm32h725ig.html)). The firmware compiles Release builds at `-O3` with hard-float FPv5-D16 ([toolchain configuration](../../channel_card/cmake/gcc-arm-none-eabi.cmake)).

Audio runs at 48 kHz in 1 ms DMA half-buffers of 48 frames ([audio bridge](../../channel_card/Core/Src/audio/audio_bridge.c)). The DMA callback currently renders the active note bank, per-voice filters, and programmed envelopes. With all eight envelopes active, native code can already calculate up to **8 x 48,000 = 384,000 envelope amplitudes/s** ([note-bank hot path](../../channel_card/Core/Src/audio/note_bank.c), [envelope implementation](../../channel_card/Core/Src/audio/note_envelope.c)). That does not prove an interpreter can run arbitrary algorithms at the same rate, but it is strong evidence that 24,000 simple control results/s is not intrinsically demanding.

The current Release map is much tighter in RAM than the MCU headline suggests:

| Region | Capacity | Occupied/reserved | Nominally unassigned |
|---|---:|---:|---:|
| Flash | 1,048,576 B | ~115,344 B load image | ~933,232 B |
| DTCM | 131,072 B | ~116,008 B | ~15,064 B |
| AXI SRAM | 327,680 B | ~300,248 B | ~27,432 B |
| D2/D3/ITCM | 114,688 B | BODY rings consume nearly all | ~320 B |

These are linker-map figures, not a guarantee that every nominal byte is safe to turn into a VM heap ([linker script](../../channel_card/STM32H725xG_flash.ld), [Release map](../../channel_card/build/Release/channel_MCU.map)). The practical conclusion is: **flash is plentiful; runtime RAM is not**. A language advertising a “small binary” can still fail because its heap, stack, module representation, or loaded bytecode needs tens of kilobytes.

There is also a hardware distinction: DAC channels 2-4 are three channel-global analogue CV outputs after the voices are mixed to DAC channel 1 ([Channel Card overview](../../channel_card/README.md)). The card can maintain 8 x N **internal per-voice DSP parameters**, but the existing hardware does not expose 24 independent analogue CV outputs.

## Why streaming the values is the wrong path

RS-485 is 921600 baud, 8N1 ([wire protocol](../../channel_card/docs/protocol.md)). Ten wire bits are needed per payload byte, so its absolute ceiling is 92,160 B/s:

| Control format | 3 params | 10 params | RS-485 result |
|---|---:|---:|---|
| float32 | 96,000 B/s | 320,000 B/s | initial case already impossible before framing |
| 16-bit | 48,000 B/s | 160,000 B/s | initial possible but expensive; future impossible |

The USB audio carrier is 10 channels x 2 bytes x 51 frames/ms = **1,020 B/ms** ([USB stream contract](../../channel_card/USB_APP/usb_stream.h)). A full-speed isochronous endpoint permits at most 1,023 bytes per frame under the USB 2.0 endpoint rules ([USB-IF USB 2.0 specification](https://www.usb.org/document-library/usb-20-specification)). Only three bytes remain in the current packet—far short of the 96 bytes/ms initially required or 320 bytes/ms later.

A separate USB endpoint could be designed, but it would preserve host scheduling jitter and compete with an intentionally dense BODY stream. Upload-once/local-execute avoids that recurring bandwidth entirely.

## Real-time manipulation while the program runs

The program must expose a fixed set of **live controls** in its compiled manifest. For an LFO these could be `rate_hz`, `depth`, `offset`, `shape`, `phase_reset`, and a modulation destination. Envelope attack/decay/release times, curve, filter amount, and per-voice values work the same way.

The host changes these controls; it does not stream the LFO's continuously changing output. For example:

```text
control rate_hz = 2.0 range 0.01..40 smooth 10ms
control depth   = 0.5 range 0..1     smooth 5ms

state phase[8]

tick(dt, input, out) {
    for voice in 0..7 {
        phase[voice] = wrap(phase[voice] + rate_hz * dt)
        out[voice].cutoff_mod = depth * sin_turns(phase[voice])
    }
}
```

Changing `rate_hz` or `depth` becomes a small sparse control message. The LFO phase and output continue locally on every tick, even if the host is briefly late or disconnected. Typical knob/UI traffic at tens or hundreds of changes per second is therefore small compared with streaming 24,000 outputs/s.

The card should implement live updates through a bounded **control mailbox**, not by letting the host or console write directly into VM memory:

- The compiled program publishes control IDs, names, types, ranges, defaults, smoothing policy, and whether each control is global or per voice.
- USB CDC or RS-485 writes `(program version, control ID, value, sequence, apply tick/ramp)` into a fixed-size single-producer/single-consumer event ring.
- At the start of a control tick, the runner drains a bounded number of pending events into the inactive input block, then gives the program one immutable snapshot. A value can never change halfway through `tick`.
- Continuous controls are clamped and smoothed natively. Trigger controls such as gate, retrigger, phase reset, and note-on are queued events so short pulses are not lost.
- Optional `apply_tick` timestamps allow several changes to land together. Late messages apply immediately or at the next tick according to an explicit policy.
- Queue overflow, stale program versions, unknown IDs, and out-of-range values are counted and rejected without disturbing audio.

For a normal sub-audio LFO, a 1 kHz program tick plus 48-sample native interpolation is appropriate. A script LFO approaching hundreds of hertz will be poorly represented and can alias; audio-rate modulation should use a native oscillator whose frequency/depth are controlled by the script. This separation keeps manipulation responsive without turning the VM into the sample-rate DSP engine.

## CPU and signal-rate budget

At 550 MHz, one millisecond contains **550,000 gross cycles**. One output block is only 96 bytes initially or 320 bytes at ten parameters; double buffering uses 192 or 640 bytes. Copying results is negligible. The unknown is how many interpreted operations each algorithm performs and how predictable the runtime is.

For the proof of concept, set an engineering target of **no more than 50-100 us measured maximum runtime per tick** under full load. That provides 27,500-55,000 cycles for control evaluation while leaving most of the millisecond for audio, USB, and console work. This is a target to validate on the board, not a claimed benchmark.

A 1 kHz control rate provides 1 ms time resolution and cannot represent modulation above 500 Hz. It is appropriate for envelopes, LFOs, curves, sequencers, and slowly changing coefficients if native code interpolates or slews each setpoint across the next 48 audio samples. Audio-rate FM and general audio DSP should remain native at 48 kHz or receive a separate AOT/DSP assessment.

## Runtime options

| Option | Fit here | Strength | Main problem |
|---|---|---|---|
| Bounded custom VM | **Best production fit** | deterministic time/memory; narrow safe API | compiler/VM engineering work |
| Berry bytecode | **Best quick prototype** | explicitly MCU-oriented; small claimed footprint | mark-sweep GC and runaway-budget integration |
| WAMR / WebAssembly | **Promising second experiment** | validation, memory sandbox, multiple source languages, AOT option | RAM and integration complexity; hard tick budget needs proof |
| Lua | Trusted development mode | mature embedding API, bytecode, custom allocator/count hooks | GC, RAM, versioned/unsafe binary chunks |
| MicroPython | Poor fit | familiar syntax, `.mpy`, native emitters | heap/float behavior and unnecessary runtime surface |
| Cython | Not applicable | fast CPython extensions | requires the CPython runtime |
| Uploaded native ARM | Vendor-only later | maximum performance | a bad pointer can corrupt or crash the firmware |

### Recommended: a bounded control VM

Use a friendly Lua-like or expression syntax on the host, but compile it into a purpose-specific instruction set. A useful initial VM only needs:

- float32 constants and arithmetic;
- fixed state registers;
- comparisons, `select`, `min`, `max`, `clamp`;
- a few native intrinsics such as phase/LFO, curve, and bounded lookup;
- manifest-declared live controls and read-only per-tick control snapshots;
- fixed input and `out[8][P]` arrays; and
- loops whose maximum trip count is known during validation.

Exclude recursion, arbitrary jumps, dynamic allocation, strings/tables during `tick`, pointers, hardware access, and unbounded loops. Validate the maximum operations before activation and enforce a runtime budget as a second line of defence. This is enough for envelope/LFO/modulation state machines without importing the nondeterminism of a general dynamic language.

### Berry

Berry is an ANSI C99 register VM designed for microcontrollers. Its project reports an interpreter core below 40 KiB and operation with less than 4 KiB heap in one Cortex-M4 configuration; it supports bytecode files and uses mark-sweep GC ([Berry project](https://github.com/berry-lang/berry)). It can also move precompiled structures into firmware to reduce RAM, though uploaded modules are a different lifecycle ([Berry advanced features](https://berry.readthedocs.io/en/latest/source/en/Chapter-8.html)). An observability hook has been used to terminate infinite loops, but the published example checks at a very coarse instruction cadence and must be tightened for a 1 ms system ([Berry discussion #430](https://github.com/berry-lang/berry/discussions/430)).

Berry is worth a prototype with a fixed allocator arena, protected calls, preallocated input/output objects, and allocation forbidden during `tick`. Its upstream footprint is not a promise that the integrated build fits or meets deadlines.

### WAMR / WebAssembly

WAMR supports Cortex-M7, classic and fast interpreters, and AOT. The project reports approximately 56.3 KiB classic-interpreter text, 58.9 KiB fast-interpreter text, or 29.4 KiB AOT runtime text in a Cortex-M4F configuration ([WAMR project](https://github.com/bytecodealliance/wasm-micro-runtime)). It can constrain all runtime allocations to a caller-provided pool ([WAMR memory model](https://bytecodealliance.github.io/wamr.dev/blog/the-wamr-memory-model/)), and a simple no-growth embedded module can use linear memory below 64 KiB ([WAMR heap tuning](https://bytecodealliance.github.io/wamr.dev/blog/understand-the-wamr-heaps/)). WebAssembly validation and bounds-checked linear memory offer a real sandbox ([WebAssembly security model](https://webassembly.org/docs/security/)).

Use the full validator and bounds checks. WAMR explicitly warns that its mini-loader skips integrity checks, and its AOT runtime trusts AOT files by default unless the optional AOT validator is enabled ([WAMR build options](https://github.com/bytecodealliance/wasm-micro-runtime/blob/main/doc/build_wamr.md)). AOT files should be built with the same pinned runtime/compiler version ([WAMR AOT compatibility](https://github.com/bytecodealliance/wasm-micro-runtime/blob/main/doc/build_wasm_app.md)).

WAMR is attractive if “script language” really means “let developers submit C/Rust/AssemblyScript modules.” For this card, test a tiny no-libc module first; general published workloads use far more memory than is currently free.

### Lua, MicroPython, and Cython

Lua is genuinely embeddable and bytecode-based. It has configurable allocation, incremental/generational GC, count hooks, and can be built with 32-bit integers and floats. But Lua warns that executing maliciously constructed binary chunks can crash the interpreter, and its VM bytecode changes across releases ([Lua 5.4 manual](https://www.lua.org/manual/5.4/manual.html), [Lua version history](https://www.lua.org/versions.html)). A stripped, fixed-arena Lua build can be a trusted developer experiment; it should not receive arbitrary uploaded chunks.

MicroPython can cross-compile `.mpy` bytecode and load ARMv7E-M native modules, but its documentation highlights heap pressure/fragmentation and notes that some ports allocate floating-point objects on the heap; Viper does not optimize floating point ([MicroPython optimization guide](https://docs.micropython.org/en/latest/reference/speed_python.html), [native `.mpy` modules](https://docs.micropython.org/en/latest/develop/natmod.html)). That is a poor match for this RAM map and deadline.

Cython translates Python-like source into C/C++ extension modules that execute inside CPython ([Cython overview](https://docs.cython.org/en/latest/src/quickstart/overview.html)). It is not a small standalone language runtime for this card.

## Safe integration design

1. **Compile on the host.** Emit bytecode plus a format version, control ABI version, live-control manifest, declared state/output limits, length, CRC, and optionally a signature.
2. **Upload via USB CDC.** Use a chunked binary operation similar in spirit to attack-head upload. Do not use RS-485 or the UAC BODY endpoint for program data.
3. **Stage, validate, then switch.** Receive into an inactive bounded slot; validate it again on the card; run `init` and dry ticks; atomically select it only on success. Keep a known-good program.
4. **Accept sparse live updates.** Resolve a control name on the host, then send compact ID/value or trigger events into a fixed queue. Reject updates for a stale program version.
5. **Never call the VM from an audio ISR.** The 1 ms callback should only signal a tick and consume the last completed output block.
6. **Run at low priority with fixed memory.** Drain a bounded number of live events, snapshot immutable controls/note/gate/time inputs, evaluate into the inactive 8 x P block, and forbid tick-time allocation.
7. **Sanitize output natively.** Reject NaN/Inf, clamp ranges, slew/interpolate across 48 samples, then atomically publish.
8. **Fail musically.** On a late/faulted tick, hold the previous values or crossfade to safe defaults. Disable the program after repeated overruns while audio and BODY continue.
9. **Measure everything.** Report current/max DWT cycles, operation count, late ticks, control-queue overflow/stale updates, faults, invalid outputs, program hash/version, arena high-water mark, I2S fill-late, and USB drop/hold counters.

CRC protects against accidental corruption, not a hostile uploader. If third-party uploads are in scope, add authenticated transport or signed programs and a rollback policy.

## Proof-of-concept plan and go/no-go gate

### Stage A — prove scheduling with native C

Implement the worst expected control algorithm in C using the proposed low-priority double buffer. Measure it with eight voices, eight filters, sustained BODY, CDC traffic, and RS-485 traffic. This separates scheduling feasibility from VM questions.

### Stage B — minimal uploaded VM

Add a RAM-only upload, about a dozen operations, fixed state, compile-time-bounded loops, CRC/version checks, and a hard operation/cycle budget. Defer persistent flash and a rich standard library.

Also add four live-control types: smoothed float, stepped integer/enum, boolean gate, and one-shot trigger. Exercise LFO rate/depth changes and phase reset while the program is running.

### Stage C — language shootout only if needed

Run identical kernels in the custom VM and Berry. Add WAMR only if multi-language sandboxing is a genuine product requirement. Use:

- eight envelope/curve state machines;
- eight LFO + envelope + clamp chains; and
- the worst valid branch/loop program.

Proceed only if:

- every accepted program has an enforced termination bound;
- measured maximum control time is <=100 us under full integrated load;
- I2S fill-late remains zero and USB BODY drop/hold counters do not regress in a sustained soak;
- the fixed arena fits without reducing the existing 16 KiB reserved stack or colliding with SRAM allocations;
- infinite-loop, NaN/Inf, oversized, truncated, bad-CRC, and wrong-version programs fail while audio continues;
- rapid live-control changes are coherent at tick boundaries, trigger events are not lost, stale updates are rejected, and smoothing produces no audible zippering; and
- upload, validation, and activation never block the audio callback.

## Final recommendation

Approve a proof of concept. Start with the scheduling benchmark and a minimal bounded VM, not a full Lua or Python integration. The 24k/80k output-rate requirement is comfortably plausible; recurring output transport is the part that is already infeasible. Let the host manipulate sparse named controls in real time while the program generates the continuous LFO/envelope/modulation locally. Keep the high-rate audio engine native, let the uploaded program generate 1 kHz targets or coefficients, and make deterministic failure behavior part of the runtime ABI from day one.

The remaining uncertainty is measurable rather than conceptual: current full-load CPU duty was not captured in this research, and upstream Berry/WAMR footprints are not this exact build. The board benchmark and fresh linker map are therefore the acceptance gate.
