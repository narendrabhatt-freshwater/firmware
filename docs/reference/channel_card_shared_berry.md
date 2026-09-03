# Channel Card shared Berry qualification

Measured on 3 September 2026 with the ABI7 event-driven envelope program:

| Measurement | Result |
| --- | ---: |
| Eight independently loaded program objects | 17,224 B allocator peak |
| Atomic replacement peak | 18,272 B |
| Upload scratch | 4,096 B |
| Formula result (peak + 20% + scratch, 1 KiB rounded) | 26,624 B |
| Final `ScriptBerryRuntime` linker section | 23,592 B |
| DTCM upload scratch + eight key maps | 5,120 B |
| Complete ARM firmware flash | 223,008 B |
| AXI SRAM used / capacity | 323,840 / 327,680 B |
| AXI SRAM remaining | 3,840 B |
| DTCM used / capacity | 117,840 / 131,072 B |

The final linker map retains a 2 KiB AXI guard. `.attack_bank` remains 262,144
bytes. The BODY configuration remains three banks of 4,080 samples per voice:
`STREAM_RING_SAMPLES == 12,240`; D2, D3, and ITCM ring sections are unchanged.

The host qualification loaded eight separately instantiated program objects,
performed 32 atomic replacement/GC cycles plus a corrupt replacement, and
completed one million mixed handler dispatches. Handler allocation and GC
counts remained zero and the active-output hash was stable. Native-argument
and non-finite input errors were voice-local; handler allocation and watchdog
runaway invalidated the shared VM and silenced every program as designed.

The compiler requires `on_note_on(key, velocity)`, `on_note_off(has_pending)`, and
`on_ramp_end()` and accepts optional `on_init()` at top level. When omitted it
embeds standard C4 tuning and the identity 128-byte map. Imports, classes,
and script globals are rejected, and the temporary handler globals are cleared
after the closures are captured in that voice's rooted object.
The linker retains the existing 16 KiB native stack reserve.

All sanctioned envelope transition paths measured at most 50 Berry
instructions. The handler limit is 64 instructions: 50 plus headroom, rounded up
to the 32-instruction watchdog quantum. The eight-handler boundary limit is
512 instructions, alongside the 55,000-cycle aggregate ceiling.

Hardware USB/audio soak testing remains a board-level acceptance activity; it
cannot be established by the desktop or linker probes.
