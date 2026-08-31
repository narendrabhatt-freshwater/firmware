# Freshwater script wire contract

`freshwater::vm` now contains only the shared FWSC container constants, Channel
Berry ABI3 enums, diagnostics types, and CRC32 implementation used by firmware
and host tooling. The interpreter and fixed allocator live in `script_runtime`.

FWVM/ABI4 bytecode is intentionally unsupported. Programs are RAM-only and
must be recompiled as FWSC Berry bytecode after this migration.
