# Freshwater script wire contract

`freshwater::vm` now contains only the shared FWSC container constants, Channel
Berry ABI6 enums, fixed 12-TET table, diagnostics types, and CRC32 used by firmware
and host tooling. The interpreter and fixed allocator live in
`cmi_core/runtime`.

FWVM/older-ABI bytecode is intentionally unsupported. Programs are RAM-only and
must be recompiled as FWSC Berry bytecode after this migration.
