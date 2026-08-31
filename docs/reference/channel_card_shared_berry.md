# Channel Card shared Berry qualification

Measured on 31 August 2026 with the event-driven envelope program:

| Measurement | Result |
| --- | ---: |
| Eight independently loaded program objects | 15,112 B allocator peak |
| Atomic replacement peak | 15,928 B |
| Upload scratch | 4,096 B |
| Formula result (peak + 20% + scratch, 1 KiB rounded) | 23,552 B |
| Final `ScriptBerryRuntime` linker section | 24,576 B |
| Berry ARM relocatable contribution probe | 63,259 B |
| Complete ARM firmware flash (`text + data`) | 163,324 B |
| AXI SRAM used / capacity | 324,824 / 327,680 B |
| AXI SRAM remaining | 2,856 B |

The final linker map retains a 2 KiB AXI guard. `.attack_bank` remains 262,144
bytes. The BODY configuration remains three banks of 4,080 samples per voice:
`STREAM_RING_SAMPLES == 12,240`; D2, D3, and ITCM ring sections are unchanged.

The host qualification loaded eight separately instantiated program objects,
performed 32 atomic replacement/GC cycles plus a corrupt replacement, and
completed one million mixed handler dispatches. Handler allocation and GC
counts remained zero and the active-output hash was stable. Native-argument
and non-finite input errors were voice-local; handler allocation and watchdog
runaway invalidated the shared VM and silenced every program as designed.

The compiler accepts only the three top-level handler definitions. Imports,
classes, and script globals are rejected, and the temporary handler globals
are cleared after the closures are captured in that voice's rooted object.
The linker retains the existing 16 KiB native stack reserve.

All sanctioned envelope transition paths measured at most 36 Berry
instructions. The handler limit is 64 instructions: 36 plus 25%, rounded up
to the 32-instruction watchdog quantum. The eight-handler boundary limit is
512 instructions, alongside the 55,000-cycle aggregate ceiling.

Hardware USB/audio soak testing remains a board-level acceptance activity; it
cannot be established by the desktop or linker probes.
