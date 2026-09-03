# Channel Card shared Berry qualification

Measured on 3 September 2026 with ABI1 envelope, oscillator, 64-state, and
large-payload qualification programs:

| Measurement | Result |
| --- | ---: |
| Eight independently loaded envelope programs | 15,808 B allocator peak |
| Envelope-program atomic replacement peak | 17,224 B |
| Eight generated 6.7 KiB-container program objects plus 32 reloads | 67,256 B peak |
| Berry heap / upload scratch | 81,920 B / 16,384 B |
| Large-program headroom over measured peak | 21.8% |
| Final `.vm_arena` linker section, including scratch | 100,880 B |
| Eight-oscillator handler | 49 Berry instructions |
| Ten-oscillator handler | 53 Berry instructions |
| Complete ARM firmware flash | 167,680 B |
| AXI SRAM used / capacity | 269,664 / 327,680 B |
| AXI SRAM remaining | 58,016 B |
| DTCM linker used / capacity | 63,872 / 131,072 B |
| DTCM linker-unassigned margin | 67,200 B |
| Runtime heap ceiling before protected stack | 67,712 B |

The upload scratch now shares the explicit AXI `.vm_arena` placement instead
of consuming DTCM. `.attack_bank` remains 131,072 bytes (256 × 512 signed-int8
samples), and each voice retains one contiguous 4,080-sample signed-int8 BODY
ring in DTCM. Oscillator descriptors have no fixed array or count: each
`osc()` dynamically allocates one descriptor from the firmware heap, regardless
of table length or how many other oscillators use that table. The heap grows in
DTCM up to the existing protected 16 KiB stack boundary. D2, D3, and ITCM
remain unused by these reserves.

The host qualification loaded eight separately instantiated program objects,
performed 32 atomic replacement/GC cycles plus a corrupt replacement, loaded
and repeatedly replaced eight generated programs beyond the former 4 KiB cap, and
completed one million mixed handler dispatches. Handler allocation and GC
counts remained zero and the active-output hash was stable. Native-argument
and non-finite input errors were voice-local; handler allocation and watchdog
runaway invalidated the shared VM and silenced every program as designed.

The compiler requires `on_note_on(key, velocity)`, `on_note_off()`, and
`on_ramp_end()` at top level. Imports, classes, and script globals are rejected,
and the temporary handler globals are cleared
after the closures are captured in that voice's rooted object.
At runtime, `pitch_for_key(key)` optionally reads standard MIDI pitch with
A4 = 440 Hz. `osc(wave, frequency)` appends an oscillator from logical
wavetable `0..7` (reserved attack-bank ID `248..255`) and returns an opaque
note-local handle. It does not allocate a Berry object, but it does allocate a
firmware-side oscillator descriptor.
`start_note()` commits the default pending pitch, while
`start_note(frequency)` atomically overrides it with a positive Hz value and
commits the note. Direct frequency selection does not depend on the lookup.
The linker retains the existing 16 KiB native stack reserve.

All sanctioned envelope and two-, eight-, and ten-oscillator paths measured at most 53 Berry
instructions. The handler limit remains the next 32-instruction watchdog
boundary at 64 instructions. The eight-handler boundary limit is
512 instructions, alongside the 55,000-cycle aggregate ceiling.

Hardware USB/audio soak testing remains a board-level acceptance activity; it
cannot be established by the desktop or linker probes.
