# RS485 Console — `rs485_console`

Standalone PC-side console for the Channel/Effect Card RS485 bus. Talks
directly to either or both boards over RS485 through a PC-side adapter
(ADAM-4520, or a generic USB↔RS485 dongle) — **no dependency on either
board's USB CDC connection.** Unplug USB entirely and this still works, as
long as the RS485 adapter is on the bus.

See [`../../docs/rs485_console_architecture.md`](../../docs/rs485_console_architecture.md)
for the full protocol design and how this fits with the on-firmware side
(`Console_Poll()` / `RS485_IsForMe()` / `RS485_Reply()` in each card's
`main.c`).

Usually driven through the `fw` CLI (`fw rs485 console|send|list|build` —
see the top-level [`../../README.md`](../../README.md) §6); this README
covers the standalone binary directly.

## Build

```bash
cd apps/console
cmake -S . -B build
cmake --build build
```

Produces `build/rs485_console`. Plain C++17, no external dependencies —
one serial backend per OS family (POSIX `termios` for macOS/Linux, Win32
comm API for Windows), selected automatically by `CMakeLists.txt`.

> The Windows backend (`src/serial_port_win32.cpp`) is written against the
> documented Win32 comm API but hasn't been run against real hardware yet
> (no Windows box + RS485 adapter in the loop as of this writing) — treat
> it as "should work," verify before relying on it.

## Usage

```
rs485_console --list
rs485_console --port <path> [--baud 115200] [--target channel|effect|all]
rs485_console --port <path> send <channel|effect|all> "<command...>"
```

| Flag | Meaning |
|---|---|
| `--port PATH` | Serial device for the RS485 adapter (required unless `--list`) |
| `--baud N` | Baud rate, default `115200` (matches both cards' UART) |
| `--target T` | Default REPL target: `channel`\|`effect`\|`all` (default `all`) |
| `--timeout-ms N` | Per-attempt reply wait, default `300` |
| `--retries N` | Extra send attempts if no reply arrives, default `2` |
| `--manual-rts` | Toggle RTS around each transmit — only for USB-RS485 dongles without auto-direction. Most modern dongles (and the ADAM-4520) don't need this. |
| `--list` | List likely serial ports for this OS and exit |

### Interactive REPL

```
$ rs485_console --port /dev/cu.usbserial-XXXX
rs485_console — target: all ('card channel|effect|all' to change, 'quit' to exit)
[all]> status
[C] CH1: USB stream...
...
[all]> card channel
(default target -> channel)
[channel]> help
...
[channel]> quit
```

- `card channel|effect|all` changes the default target for subsequent
  lines (not sent to the bus — no firmware command named `card` exists).
- Any other line is sent as a console command (`help`, `status`, `sw
  bypass on`, ...) to the current default target.
- An explicit `c:`/`e:`/`*:` prefix on a line overrides the default target
  for that one line, same as typing it directly on the bus.
- `[C]`/`[E]` reply tags are colorized when stdout is a TTY, so broadcast
  (`*:`) replies from both cards are easy to tell apart.
- No reply within the timeout window is reported as "bus busy or no card
  listening," not a hard error — this mirrors `RS485_WaitBusFree()` on the
  firmware side: a single missed window on a shared, collision-avoided bus
  means "try again," not "gone." The tool retries automatically
  (`--retries`) before giving up.

### One-shot mode

```
rs485_console --port /dev/cu.usbserial-XXXX send channel status
```

Sends one command, prints the reply, exits. Exit code `1` if no reply
arrived after retries.

## Design notes

- **Wire protocol mirrors the firmware exactly**: CRLF-terminated lines,
  `c:`/`e:`/`*:` addressing, `[C] `/`[E] `-tagged replies. See
  `src/rs485_link.hpp` for the implementation and the code comments
  cross-referencing the firmware side.
- **Adapter-agnostic by design**: the actual RS485↔PC adapter wasn't
  connected yet when this was written, so nothing here assumes a specific
  model. `--list` enumerates candidates; `--manual-rts` is an opt-in
  fallback for dongles that need it. Point `--port` at whatever shows up.
- **No background threads / async I/O.** RS485 is half-duplex with one
  transmitter at a time, so the console only ever has one request in
  flight — a simple blocking-with-timeout `SerialPort` is enough (see
  `src/serial_port.hpp`).
