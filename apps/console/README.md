# RS485 Console — `rs485_console`

PC-side front-end for the Channel Card’s **`N0`…`NF`** note bank (and
`gain`) over a USB↔RS485 adapter (no board USB CDC required).

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
| `N0`…`NF` `0` | that note off (gain/bypass unchanged) |
| `N0 440` / `440` | note 0 @ 440 Hz, scale **1.0** (bare number → n0) |
| `N0 440 0.5` | note 0 @ 440 Hz, amplitude 0.5 |
| `N1 550` | note 1 @ 550 Hz, scale 1.0 (summed with other notes) |
| `N2 660 0.1` | note 2 @ 660 Hz, amplitude 0.1 |
| `gain 1 40` | CH1 DAC atten −40 dB (any ch 1..4, 0..127 dB) |

```bash
fw rs485 list
fw rs485 send channel "N0 440 0.5" --port /dev/cu.usbserial-XXXX
fw rs485 channel --port /dev/cu.usbserial-XXXX
```

Default target is **channel**. Effect Card still has its own console if you
`card effect` / `--target effect`.
