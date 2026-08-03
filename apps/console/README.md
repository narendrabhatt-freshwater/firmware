# RS485 Console — `rs485_console`

Optional PC client for the Channel/Effect RS485 bus. **Any serial terminal**
at 115200 8N1 can speak the same ASCII protocol (`screen`, `minicom`, PuTTY);
this tool adds targeting, retries, colors, and `--echo-off`.

Protocol: [`../../docs/rs485_console_architecture.md`](../../docs/rs485_console_architecture.md).
Shared library: [`../../libs/rs485`](../../libs/rs485).
Usually driven as `fw rs485 …` (top-level README §6).

## Build

```bash
cd apps/console
cmake -S . -B build
cmake --build build
```

## Options

| Flag | Meaning |
|------|---------|
| `--port PATH` | USB↔RS485 adapter |
| `--target channel\|effect\|all` | Default address prefix |
| `--echo-off` | Send `e:echo off` at start |
| `--timeout-ms` / `--retries` | Strict ACK wait / retries on timeout |

With device echo off, enable **local** echo in a plain terminal.

## Commands (typeable from any terminal too)

| Input | Meaning |
|-------|---------|
| `e:echo off` | Disable Effect keystroke bus echo |
| `c:n0` | Session defaults (bypass + gain 1 0; clears quiet) |
| `c:n3 440` | Note 3 @ 440 Hz (scale 0.125 if omitted) |
| `c:n3 0` | Note 3 off |
| `c:silence` | All notes off |
| `c:gain 1 6` | CH1 atten −6 dB |
| `c:quiet off` | Restore RS485 replies if a prior session left quiet on |

Replies are compact: `[C]ok` / `[C]err:range` (CRLF).

```bash
fw rs485 list
fw rs485 --port /dev/cu.usbserial-XXXX --echo-off
fw rs485 send channel "n0 440" --port /dev/cu.usbserial-XXXX --echo-off
```
