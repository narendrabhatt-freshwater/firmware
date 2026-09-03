# Channel Card shared Berry qualification

Measured on 3 September 2026 with the ABI1 event-driven envelope program:

| Measurement | Result |
| --- | ---: |
| Eight independently loaded program objects | 15,768 B allocator peak |
| Atomic replacement peak | 17,184 B |
| Upload scratch | 4,096 B |
| Formula result (peak + 20% + scratch, 1 KiB rounded) | 25,600 B |
| Final `ScriptBerryRuntime` linker section | 22,536 B |
| DTCM upload scratch | 4,096 B |
| Complete ARM firmware flash | 222,408 B |
| AXI SRAM used / capacity | 322,784 / 327,680 B |
| AXI SRAM remaining | 4,896 B |
| DTCM used / capacity | 116,792 / 131,072 B |

The final linker map retains a 2 KiB AXI guard. `.attack_bank` remains 262,144
bytes. The BODY configuration remains three banks of 4,080 samples per voice:
`STREAM_RING_SAMPLES == 12,240`; D2, D3, and ITCM ring sections are unchanged.

The host qualification loaded eight separately instantiated program objects,
performed 32 atomic replacement/GC cycles plus a corrupt replacement, and
completed one million mixed handler dispatches. Handler allocation and GC
counts remained zero and the active-output hash was stable. Native-argument
and non-finite input errors were voice-local; handler allocation and watchdog
runaway invalidated the shared VM and silenced every program as designed.

The compiler requires `on_note_on(key, velocity)`, `on_note_off()`, and
`on_ramp_end()` at top level. Imports, classes, and script globals are rejected,
and the temporary handler globals are cleared
after the closures are captured in that voice's rooted object.
At runtime, `pitch_for_key(key)` optionally reads standard MIDI pitch with
A4 = 440 Hz.
`start_note()` commits the default pending pitch, while
`start_note(frequency)` atomically overrides it with a positive Hz value and
commits the note. Direct frequency selection does not depend on the lookup.
The linker retains the existing 16 KiB native stack reserve.

All sanctioned envelope transition paths measured at most 47 Berry
instructions. The handler limit is 64 instructions: 47 plus headroom, rounded up
to the 32-instruction watchdog quantum. The eight-handler boundary limit is
512 instructions, alongside the 55,000-cycle aggregate ceiling.

Hardware USB/audio soak testing remains a board-level acceptance activity; it
cannot be established by the desktop or linker probes.
