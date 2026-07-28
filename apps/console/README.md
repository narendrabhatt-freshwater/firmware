# RS485 Console — `rs485_console`

PC-side front-end for the Channel Card’s sole RS485/USB command **`N0`**,
via a USB↔RS485 adapter (no board USB CDC required).

Protocol / bus details:
[`../../docs/rs485_console_architecture.md`](../../docs/rs485_console_architecture.md).
Usually driven as `fw rs485 …` (top-level [`../../README.md`](../../README.md) §6).

## Build

```bash
cd apps/console
cmake -S . -B build
cmake --build build
```

## Command

| Input | Meaning |
|---|---|
| (REPL start) / `N0` | bypass on + gain 1 0 |
| `N0 0` or `0` | tone off (gain/bypass unchanged) |
| `N0 1000.5` or `1000.5` | CH1 tone @ 1000.5 Hz (gain/bypass unchanged) |
| `gain 1 40` | CH1 DAC atten −40 dB (any ch 1..4, 0..127 dB) |

```bash
fw rs485 list
fw rs485 send channel "N0 1000.5" --port /dev/cu.usbserial-XXXX
fw rs485 channel --port /dev/cu.usbserial-XXXX
```

Default target is **channel**. Effect Card still has its own console if you
`card effect` / `--target effect`.
