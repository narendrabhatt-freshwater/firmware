# RS485 Console

Host tool for Freshwater Channel / Effect cards over multi-drop RS485.
Any serial terminal can type the same ASCII commands; this tool adds
targeting, retries, colors, and `--echo-off`.

## Build

See repo root / `fw` wrapper.

## Flags

| Flag | Action |
| --- | --- |
| `--echo-off` | Send `e:ec 0` at start |
| `--port PATH` | Serial adapter |
| `--baud N` | Baud (default matches cards) |

With device echo off, enable **local** echo in a plain terminal.

## Short commands (current firmware)

| Command | Action |
| --- | --- |
| `e:ec 0` | Disable Effect keystroke bus echo |
| `c:h` | Channel command list |
| `e:h` | Effect command list |
| `c:n0` | Session defaults (bypass + `g 1 0`) |
| `c:n0..nf <Hz> [sc]` | Note bank |
| `c:f0..f7 <Hz>` / `c:f <Hz>` | LPF on voices 0..7 |
| `c:n 0` | All notes off |
| `c:g 1 6` | CH1 atten −6 dB |
| `c:cpu` / `c:cpu 0` | LED_Y load probe on / off |

```bash
fw rs485 --port /dev/cu.usbserial-XXXX --echo-off
fw rs485 send channel "n0 440" --port /dev/cu.usbserial-XXXX --echo-off
```
