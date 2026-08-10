# Freshwater protocol stack (PC host side)

Everything a PC needs to talk to the Channel and Effect cards: the wire
API, the transports, and the two host apps built on them. The normative
wire contract is `docs/protocol.md` in the product tree (a copy ships
with releases of this repository).

| Directory | Contents |
| --------- | -------- |
| `libs/cardproto/` | Wire API only — command formatters, reply parsing, typed `ChannelClient` / `EffectClient` over an injected transport. No I/O. |
| `libs/cardlink/` | OS transports — serial ports (POSIX/Win32), tagged RS485 link + `Bus`, USB CDC console + `al`/`bl` sample upload. Depends on cardproto. |
| `midi_bridge/` | MIDI keyboard → 8-voice FIFO allocator → Mac speakers (RtAudio) or Channel Card notes over RS485. |
| `console/` | Standalone RS485 console REPL (`rs485_console`) — targeting, retries, `--echo-off`. |

## Build

Each app is a standalone CMake project that pulls the libs by relative
path (`add_subdirectory(../libs/cardlink)`), so the directory layout in
this repository must be preserved.

```bash
cmake -S midi_bridge -B midi_bridge/build && cmake --build midi_bridge/build
cmake -S console -B console/build && cmake --build console/build
```

C++17. `midi_bridge` fetches RtMidi/RtAudio with FetchContent on first
configure (network needed once).

## Layering

```text
midi_bridge / console / cmi_control (GUI, separate repo)
        │
   cardlink   (serial + RS485 bus + CDC upload)
        │
   cardproto  (ASCII command language — no I/O)
```

Host teams that own their own pipe can take `libs/cardproto` alone and
ship it with the protocol specification.
