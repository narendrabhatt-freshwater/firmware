# Channel Card firmware size breakdown — 17 September 2026

Source: migration commit `58371a3`, clean Release build, Arm GNU Toolchain 15.3.Rel1 (`-O3 -g0`), default RS485 configuration. Sizes come from live input sections in `channel_MCU.map`, not archive sizes or the number of source files. All figures below are bytes unless stated otherwise.

## What is in the 167,372-byte binary?

The image is **163.45 KiB**, using **15.96% of the 1 MiB flash**. Figures include executable code, read-only tables/strings and initialized data. Rows are mutually exclusive; the VM row excludes its shared C/math/compiler dependencies, which appear separately.

| Component | Flash bytes | KiB | Share |
| --- | ---: | ---: | ---: |
| VM subsystem (Berry + integration/upload) | 46,059 | 44.98 | 27.52% |
| STM32 HAL/LL drivers | 33,633 | 32.84 | 20.09% |
| C library (newlib-nano) | 21,895 | 21.38 | 13.08% |
| Audio engine / DSP | 18,625 | 18.19 | 11.13% |
| Math library | 14,232 | 13.90 | 8.50% |
| Console / RS485 / attack upload | 13,655 | 13.33 | 8.16% |
| Our USB CDC stack | 6,680 | 6.52 | 3.99% |
| Compiler support (libgcc) | 6,064 | 5.92 | 3.62% |
| Startup / peripheral init / system glue | 5,072 | 4.95 | 3.03% |
| Our DAC / LED drivers | 1,318 | 1.29 | 0.79% |
| Linker overhead/alignment not attributed to objects | 139 | 0.14 | 0.08% |
| **Total** | **167,372** | **163.45** | **100%** |

No TinyUSB/UAC code remains in this link. Host RtMidi, libserialport, `voicebdgui`, sound-library files, and host PCM buffers do not enter the MCU binary. The full Berry source tree is compiled into a static archive, but only referenced sections are retained by `--gc-sections`.

## VM flash breakdown

| VM component | Flash bytes |
| --- | ---: |
| Berry interpreter dispatch (`be_vm.c`) | 10,700 |
| Our backend: arena allocator, native operations, loading, dispatch, metrics | 9,135 |
| Berry strings (`be_string.c`) | 4,320 |
| Berry string/value conversion (`be_strlib.c`) | 3,296 |
| Berry garbage collector | 2,796 |
| Berry C API | 2,336 |
| Bytecode loader | 1,848 |
| Map implementation | 1,632 |
| Module machinery | 1,572 |
| Class machinery | 1,500 |
| Protected execution / exception machinery | 1,284 |
| Memory-management helpers | 976 |
| Remaining Berry runtime/glue | 3,312 |
| Our VM container validation/CRC (`vm.c`) | 676 |
| Our VM uploader (`vm_upload.c`) | 348 |
| Channel VM wrapper (`channel_vm.c`) | 328 |
| **Total VM subsystem** | **46,059** |

One shared Berry interpreter serves all eight voices; there are not eight separate interpreter binaries or eight separate VM heaps. Each voice has its own installed program/state. The 46,059-byte subtotal does not include all functions the VM uses indirectly from the C/math/compiler libraries.

Already disabled in `runtime/include/script/berry/berry_conf.h`: source compiler, filesystem, bytecode saver, shared-library loading, JSON, OS/time, optional math/string modules and debug modules/hooks. `be_module_table` is empty. The generated built-in table contains only `number`, `int`, `real` and `bool`. There is no on-device source compiler hiding in the final binary.

`be_strlib.c` still contributes because it also implements core value conversion and string operations outside the optional string-module switch. Likewise class/module operations remain reachable from interpreter opcode dispatch. Removing them would require defining and enforcing a smaller supported bytecode language; their presence alone does not prove they can safely be deleted. GC is used during initialization/loading even though allocation/GC in note handlers is rejected.

## VM RAM and stacks — not part of the .bin

Cross-compiled `sizeof` measurements for the actual ARM ABI confirm this allocation:

| Major VM-region allocation | RAM bytes |
| --- | ---: |
| Shared Berry heap: programs, objects, strings, globals, VM stacks | 81,920 |
| One upload scratch buffer, maximum program payload | 16,384 |
| Per-voice state: 8 voices × 64 floats × 4 bytes | 2,048 |
| Remaining runtime struct: metrics, native pointers, flags, recovery state, alignment | 528 |
| **Total `.vm_arena` NOLOAD region** | **100,880** |

Inside the **81,920-byte heap**, rather than additional allocations outside it:

- Value/register stack: **128 × 8-byte `bvalue` = 1,024 bytes**, preallocated on initialization and shared across voices.
- Call-frame array: **8 × 20-byte `bcallframe` = 160 bytes** initially reserved.
- VM object: **324 bytes**, plus other vectors, globals, strings, loaded programs and allocator overhead. Exception/reference/trace vectors are also heap-backed; their live sizes vary.
- Allocator blocks have 8-byte headers and 8-byte rounding. The above payload sizes do not include that overhead.

The separate CPU C/interrupt stack reserves **16,384 bytes for the entire firmware**, including C calls made by the VM. This is neither a per-voice stack nor flash. The linker also reserves a **512-byte minimum C heap**, not a 512-byte maximum; oscillator allocations use the general C heap, separate from Berry’s fixed arena.

Other large RAM reservations: attack storage **131,072 B** (256 × 512 samples), voice ring structs **32,832 B** (including 32,640 sample bytes), USB RX/TX arrays **8,192 + 2,048 B**, and I2S DMA buffers **1,536 B**. Static state/alignment and the heap/stack reservation bring the current combined DTCM/AXI region allocation to **308,672 B**. Uninitialized reservations do not add those bytes to `.bin`.

These are reservations and structure sizes, not measured runtime peaks. The running host currently owns RS485, so this audit did not interrupt it to query `c:vm mem`. That command reports current/peak allocated Berry payload and fragmentation. Its `arena_size` field reports 98,304 B (80 KiB heap + 16 KiB scratch), while `arena_current` counts allocated heap payload; neither is the full 100,880-byte runtime-region size.

## Candidates for reduction, with measured isolated builds

All experiments used temporary source copies under `/tmp/freshwater-usb-audit/`. No candidate was applied to working firmware or flashed. Each saving is relative to the original 167,372-byte Release image and is **not additive** without a combined rebuild. Successful linking does not establish behavioral equivalence or latency.

| Isolated experiment | Resulting .bin | Saved | Interpretation |
| --- | ---: | ---: | --- |
| Remove forced `_printf_float` / `_scanf_float` linkage | 150,204 | 17,168 | Requires replacing current floating-point parsing/formatting first; simply deleting flags breaks decimal commands and float output. Includes transitive C/compiler dependencies. |
| Compile Berry runtime and our Berry backend with `-Os` | 155,508 | 11,864 | Keeps application mixer/USB at `-O3`, but VM handler timing must be measured. |
| Compile `STM32_Drivers` with `-Os` | 158,540 | 8,832 | USB/interrupt performance must be requalified; not a free saving. |
| Remove the startup call to `MX_SAI4_Init()` | 166,180 | 1,192 | Source has SAI4 initialization but no SAI transfers. Check clock/pin/board intent before adopting; current DAC playback uses I2S. |

The largest plausible avoidable feature cost is full floating-point console I/O, not an unused network or audio middleware package. The current console explicitly relies on `%lf` scanning and fractional output, and Berry core formatting also has float paths. A lightweight replacement is a deliberate implementation change with parser/error-path tests, not a linker-flag-only cleanup.

Double-precision math contributes **14,232 B** through functions used for pitch/filter calculations (`pow`, `tan`, range reduction and tables), alongside single-precision operations. Switching selected calculations to single precision or tables may save flash, but needs a numerical/audio accuracy check. No saving for that change has been measured here.

Compiler support includes **4,024 B of ARM unwinding support** pulled transitively by newlib `setjmp`; Berry and our handler fault recovery use `setjmp`/`longjmp`. This is not evidence that a C++ application runtime was accidentally linked.

My preferred next step is to examine the unused SAI initialization and redesign costly console float I/O while preserving command behavior. Keep performance-sensitive optimization flags until measured timing supports a change. Reducing VM heap/stack reservations would save RAM, not firmware flash.

## Full per-object flash attribution

Also provided as `firmware_size_objects.csv` for sorting/filtering. Sizes include retained code, constants and initialized data, excluding discarded sections and alignment.

### VM subsystem (Berry + integration/upload)

| Linked object | Flash bytes |
| --- | ---: |
| `libberry_runtime.a(be_vm.c.obj)` | 10,700 |
| `libscript_runtime_berry_backend.a(berry_backend.c.obj)` | 9,135 |
| `libberry_runtime.a(be_string.c.obj)` | 4,320 |
| `libberry_runtime.a(be_strlib.c.obj)` | 3,296 |
| `libberry_runtime.a(be_gc.c.obj)` | 2,796 |
| `libberry_runtime.a(be_api.c.obj)` | 2,336 |
| `libberry_runtime.a(be_bytecode.c.obj)` | 1,848 |
| `libberry_runtime.a(be_map.c.obj)` | 1,632 |
| `libberry_runtime.a(be_module.c.obj)` | 1,572 |
| `libberry_runtime.a(be_class.c.obj)` | 1,500 |
| `libberry_runtime.a(be_exec.c.obj)` | 1,284 |
| `libberry_runtime.a(be_mem.c.obj)` | 976 |
| `libberry_runtime.a(be_baselib.c.obj)` | 712 |
| `libfreshwater_vm.a(vm.c.obj)` | 676 |
| `libberry_runtime.a(be_vector.c.obj)` | 624 |
| `libberry_runtime.a(be_object.c.obj)` | 540 |
| `libberry_runtime.a(be_func.c.obj)` | 432 |
| `libberry_runtime.a(be_var.c.obj)` | 400 |
| `libberry_runtime.a(be_debug.c.obj)` | 388 |
| `vm_upload.c.obj` | 348 |
| `channel_vm.c.obj` | 328 |
| `libberry_runtime.a(be_list.c.obj)` | 208 |
| `libberry_runtime.a(be_libs.c.obj)` | 4 |
| `libberry_runtime.a(berry_runtime_modtab.c.obj)` | 4 |

### STM32 HAL/LL drivers

| Linked object | Flash bytes |
| --- | ---: |
| `stm32h7xx_hal_rcc_ex.c.obj` | 5,780 |
| `stm32h7xx_hal_dma.c.obj` | 5,248 |
| `stm32h7xx_hal_pcd.c.obj` | 4,468 |
| `stm32h7xx_ll_usb.c.obj` | 3,444 |
| `stm32h7xx_hal_i2c.c.obj` | 3,152 |
| `stm32h7xx_hal_rcc.c.obj` | 3,100 |
| `stm32h7xx_hal_uart.c.obj` | 1,828 |
| `stm32h7xx_hal_tim.c.obj` | 1,452 |
| `stm32h7xx_hal_gpio.c.obj` | 1,016 |
| `stm32h7xx_hal_i2s.c.obj` | 1,012 |
| `stm32h7xx_hal_sai.c.obj` | 856 |
| `stm32h7xx_hal_uart_ex.c.obj` | 468 |
| `stm32h7xx_hal_cortex.c.obj` | 396 |
| `stm32h7xx_hal_pcd_ex.c.obj` | 324 |
| `stm32h7xx_hal.c.obj` | 313 |
| `system_stm32h7xx.c.obj` | 252 |
| `stm32h7xx_hal_tim_ex.c.obj` | 212 |
| `stm32h7xx_hal_i2c_ex.c.obj` | 172 |
| `stm32h7xx_hal_pwr_ex.c.obj` | 140 |

### C library (newlib-nano)

| Linked object | Flash bytes |
| --- | ---: |
| `libg_nano.a(libc_a-strtod.o)` | 3,076 |
| `libg_nano.a(libc_a-dtoa.o)` | 2,952 |
| `libg_nano.a(libc_a-mprec.o)` | 2,686 |
| `libg_nano.a(libc_a-nano-vfprintf_float.o)` | 1,908 |
| `libg_nano.a(libc_a-gdtoa-gethex.o)` | 1,250 |
| `libg_nano.a(libc_a-nano-vfscanf_float.o)` | 1,036 |
| `libg_nano.a(libc_a-nano-svfscanf.o)` | 924 |
| `libg_nano.a(libc_a-nano-vfprintf_i.o)` | 830 |
| `libg_nano.a(libc_a-nano-svfprintf.o)` | 688 |
| `libg_nano.a(libc_a-nano-vfscanf_i.o)` | 664 |
| `libg_nano.a(libc_a-nano-vfprintf.o)` | 630 |
| `libg_nano.a(libc_a-gdtoa-hexnan.o)` | 390 |
| `libg_nano.a(libc_a-locale.o)` | 364 |
| `libg_nano.a(libc_a-findfp.o)` | 344 |
| `libg_nano.a(libc_a-fflush.o)` | 336 |
| `libg_nano.a(libc_a-mallocr.o)` | 316 |
| `libg_nano.a(libc_a-ctype_.o)` | 257 |
| `libg_nano.a(libc_a-strtol.o)` | 248 |
| `libg_nano.a(libc_a-strtoul.o)` | 224 |
| `libg_nano.a(libc_a-makebuf.o)` | 186 |
| `libg_nano.a(libc_a-wsetup.o)` | 168 |
| `libg_nano.a(libc_a-memchr.o)` | 160 |
| `libg_nano.a(libc_a-freer.o)` | 152 |
| `libg_nano.a(libc_a-stdio.o)` | 140 |
| `libg_nano.a(libc_a-wbuf.o)` | 122 |
| `libg_nano.a(libc_a-ungetc.o)` | 116 |
| `libg_nano.a(libc_a-sccl.o)` | 114 |
| `libg_nano.a(libc_a-snprintf.o)` | 100 |
| `libg_nano.a(libc_a-signal.o)` | 96 |
| `libg_nano.a(libc_a-reallocr.o)` | 92 |
| `libg_nano.a(libc_a-sscanf.o)` | 88 |
| `libg_nano.a(libc_a-impure.o)` | 80 |
| `libg_nano.a(libc_a-init.o)` | 72 |
| `libg_nano.a(libc_a-sprintf.o)` | 68 |
| `libg_nano.a(libc_a-assert.o)` | 60 |
| `libg_nano.a(libc_a-fwalk.o)` | 58 |
| `libg_nano.a(libc_a-memmove.o)` | 50 |
| `libg_nano.a(libc_a-setjmp.o)` | 44 |
| `libg_nano.a(libc_a-strncat.o)` | 42 |
| `libg_nano.a(libc_a-signalr.o)` | 40 |
| `libg_nano.a(libc_a-callocr.o)` | 40 |
| `libg_nano.a(libc_a-strncpy.o)` | 38 |
| `libg_nano.a(libc_a-strncmp.o)` | 36 |
| `libg_nano.a(libc_a-lseekr.o)` | 36 |
| `libg_nano.a(libc_a-readr.o)` | 36 |
| `libg_nano.a(libc_a-writer.o)` | 36 |
| `libg_nano.a(libc_a-mbtowc_r.o)` | 36 |
| `libg_nano.a(libc_a-fprintf.o)` | 36 |
| `libg_nano.a(libc_a-fstatr.o)` | 36 |
| `libg_nano.a(libc_a-malloc.o)` | 32 |
| `libg_nano.a(libc_a-sbrkr.o)` | 32 |
| `libg_nano.a(libc_a-closer.o)` | 32 |
| `libg_nano.a(libc_a-isattyr.o)` | 32 |
| `libg_nano.a(libc_a-strcmp.o)` | 28 |
| `libg_nano.a(libc_a-strchr.o)` | 28 |
| `libg_nano.a(libc_a-memcpy-stub.o)` | 28 |
| `libg_nano.a(libc_a-wctomb_r.o)` | 26 |
| `libg_nano.a(libc_a-mlock.o)` | 24 |
| `libg_nano.a(libc_a-strlen.o)` | 16 |
| `libg_nano.a(libc_a-memset.o)` | 16 |
| `libg_nano.a(libc_a-strcpy.o)` | 16 |
| `libg_nano.a(libm_a-s_nan.o)` | 16 |
| `libg_nano.a(libc_a-msizer.o)` | 16 |
| `libg_nano.a(libc_a-abort.o)` | 14 |
| `libg_nano.a(libc_a-errno.o)` | 12 |
| `libg_nano.a(libm_a-sf_nan.o)` | 12 |
| `libg_nano.a(libc_a-localeconv.o)` | 8 |
| `libg_nano.a(libc_a-lock.o)` | 6 |

### Audio engine / DSP

| Linked object | Flash bytes |
| --- | ---: |
| `note_bank.c.obj` | 6,992 |
| `wavetable_osc.c.obj` | 3,036 |
| `stream_ring.c.obj` | 3,020 |
| `audio_tone_dc.c.obj` | 1,453 |
| `note_filter.c.obj` | 1,408 |
| `audio_bridge.c.obj` | 952 |
| `note_envelope.c.obj` | 664 |
| `attack_bank.c.obj` | 556 |
| `butterworth_four_pole.c.obj` | 544 |

### Math library

| Linked object | Flash bytes |
| --- | ---: |
| `libm.a(libm_a-pow_log_data.o)` | 4,168 |
| `libm.a(libm_a-exp_data.o)` | 2,160 |
| `libm.a(libm_a-k_rem_pio2.o)` | 1,796 |
| `libm.a(libm_a-pow.o)` | 1,504 |
| `libm.a(libm_a-e_rem_pio2.o)` | 1,076 |
| `libm.a(libm_a-sf_pow.o)` | 792 |
| `libm.a(libm_a-k_tan.o)` | 600 |
| `libm.a(libm_a-ef_fmod.o)` | 400 |
| `libm.a(libm_a-sf_exp2_data.o)` | 328 |
| `libm.a(libm_a-sf_pow_log2_data.o)` | 296 |
| `libm.a(libm_a-s_floor.o)` | 272 |
| `libm.a(libm_a-s_scalbn.o)` | 244 |
| `libm.a(libm_a-math_err.o)` | 232 |
| `libm.a(libm_a-math_errf.o)` | 160 |
| `libm.a(libm_a-s_tan.o)` | 96 |
| `libm.a(libm_a-wf_fmod.o)` | 88 |
| `libm.a(libm_a-s_fabs.o)` | 20 |

### Console / RS485 / attack upload

| Linked object | Flash bytes |
| --- | ---: |
| `channel_console.c.obj` | 12,283 |
| `attack_upload.c.obj` | 704 |
| `uart5_rx.c.obj` | 668 |

### Our USB CDC stack

| Linked object | Flash bytes |
| --- | ---: |
| `usb_device.c.obj` | 3,727 |
| `usb_app.c.obj` | 2,636 |
| `usb_descriptors.c.obj` | 317 |

### Compiler support (libgcc)

| Linked object | Flash bytes |
| --- | ---: |
| `libgcc.a(unwind-arm.o)` | 2,760 |
| `libgcc.a(_arm_addsubdf3.o)` | 888 |
| `libgcc.a(pr-support.o)` | 856 |
| `libgcc.a(_udivmoddi4.o)` | 800 |
| `libgcc.a(libunwind.o)` | 408 |
| `libgcc.a(_aeabi_ldivmod.o)` | 160 |
| `libgcc.a(_fixunsdfdi.o)` | 64 |
| `libgcc.a(_aeabi_uldivmod.o)` | 48 |
| `libgcc.a(_popcountsi2.o)` | 40 |
| `libgcc.a(_fixdfdi.o)` | 36 |
| `libgcc.a(_dvmd_tls.o)` | 4 |

### Startup / peripheral init / system glue

| Linked object | Flash bytes |
| --- | ---: |
| `main.c.obj` | 812 |
| `startup_stm32h725xx.s.obj` | 804 |
| `i2s.c.obj` | 728 |
| `tim.c.obj` | 668 |
| `usart.c.obj` | 360 |
| `sai.c.obj` | 328 |
| `gpio.c.obj` | 296 |
| `i2c.c.obj` | 284 |
| `usb_otg.c.obj` | 192 |
| `crtbegin.o` | 148 |
| `stm32h7xx_it.c.obj` | 120 |
| `syscalls.c.obj` | 120 |
| `sysmem.c.obj` | 80 |
| `dma.c.obj` | 72 |
| `stm32h7xx_hal_msp.c.obj` | 36 |
| `crtn.o` | 16 |
| `crti.o` | 8 |

### Our DAC / LED drivers

| Linked object | Flash bytes |
| --- | ---: |
| `cs4304.c.obj` | 1,052 |
| `channel_led.c.obj` | 266 |

