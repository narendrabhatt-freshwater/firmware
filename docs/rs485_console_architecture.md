# RS485 Console — Architecture

Half-duplex, multi-drop RS485 console on Channel and Effect Card firmware,
plus optional PC tools that speak the **same ASCII protocol** any serial
terminal can type (`screen`, `minicom`, PuTTY at **115200 8N1**).

## Shipping model

| Artifact | Role | Required? |
|----------|------|-----------|
| Channel / Effect firmware | Product protocol (addressing, commands, `ok`/`err`) | Yes |
| USB↔RS485 adapter + any terminal | Maintenance / bring-up | Enough for lab |
| [`apps/console`](../apps/console) (`rs485_console`) | Convenience REPL, retries, `--echo-off` | No |
| [`apps/midi_host`](../apps/midi_host) | MIDI → one-by-one notes via [`libs/rs485`](../libs/rs485) | No |
| [`libs/rs485`](../libs/rs485) | Shared C++17 automation client | Host apps only |

**Implication:** do not invent framing a terminal cannot type. Voice commands
are ASCII (`c:n3 440` + Enter). Compact `[C]ok` replies stay human-readable.

```mermaid
flowchart LR
  Term[Any serial terminal]
  Con[apps/console]
  Midi[apps/midi_host]
  Lib[libs/rs485]
  Bus[RS485 adapter 115200 8N1]
  CC[Channel Card]
  EC[Effect Card]
  Con --> Lib
  Midi --> Lib
  Lib --> Bus
  Term --> Bus
  Bus --> CC
  Bus --> EC
```

## Physical bus

One shared D+/D- multi-drop bus. Operators:

| Operator | Role |
|----------|------|
| Rockchip CPU card (future) / `midi_host` | Production voice/control |
| PC + terminal or `apps/console` | Maintenance |

Addresses: `c:` Channel, `e:` Effect, `*:` / bare = broadcast.

## Wire contract

| Direction | Rule |
|-----------|------|
| Host → card | One ASCII line, optional `c:`/`e:`/`*:`, **single `\r`** (never `\r\n` — dual EOL double-executes) |
| Card → host | One terminal reply: `[C]` or `[E]` + body + **`\r\n`** |
| Success | `[C]ok\r\n` / `[E]ok\r\n` (compact) |
| Failure | `[C]err:<code>\r\n` — `syntax`, `range`, `unknown`, `rxdrop`, … |

### Voice dialect (MIDI / realtime)

Integer Hz; on-card default scale **0.125** when `[scale]` omitted.

| Intent | Example TX | ACK |
|--------|------------|-----|
| Note on | `c:n3 440\r` | `[C]ok\r\n` |
| Note off | `c:na 0\r` | `[C]ok\r\n` |
| Silence | `c:n 0\r` | `[C]ok\r\n` |
| Gain | `c:g 1 6\r` | `[C]ok\r\n` |

Host keeps the 16-slot **VoiceBank**; the card only receives per-slot `nX`.
No binary bank frames on the production path.

### Echo (Effect)

`e:ec 0` / `e:ec 1` — runtime keystroke bus echo (firmware default
**off**). Production and burst TX require echo off. With device echo off,
enable **local** echo in the terminal. Any client can type `e:ec 0`
(including `rs485_console --echo-off`).

### Half-duplex

- **Single-master (PC / midi_host):** host is the lock — one command in
  flight, wait for `[C]ok` / timeout, then next. Do not Link-resend into
  a live ACK (card DE high ⇒ RX deaf ⇒ death spiral of `no cok` on Offs).
- Cards: acquire DE, one short TX, release; never drop the ACK. (PC USB
  adapter DE is not on `RS485_CTL` — cards cannot see host TX via CTL.)
- Host: write → drain → wait for tagged line; MIDI uses tiny `idle_gap`
  and `post_ack_settle_ms = 0`. On miss: RX-idle before Link retry, then
  fail-stop. Operators: `e:echo off` / `--echo-off`.
- Channel note-bank must keep the I2S DMA refill inside its half-buffer
  deadline (enable I-cache); otherwise the main loop starves and the
  console goes deaf under polyphony.

### Strict ACK / errors

Every command waits for a tagged reply. Host status: `Ok`, `Err`, `Timeout`,
`IoError`, `BadReply`. Retry only on Timeout/BadReply; never on `Err`. Never
block forever. Session bootstrap aborts if required steps fail.

## On-MCU (Channel)

[`channel_console.c`](../apps/channel_card/Core/Src/channel_console.c): UART5
IRQ RX, DE/RE turnaround, `Console_Poll` / `Console_Exec`, compact replies.
Same command set over USB CDC (no `[C]` tag on CDC).

## Host library

[`libs/rs485`](../libs/rs485): `SerialPort`, `Link` (burst TX), `Session`
(bootstrap + `SetNote` / `Silence` / `Gain`). Linked by `apps/console` and
`apps/midi_host`.

## Command reference

Live `help` / per-card READMEs remain the command-set source of truth; this
doc owns framing and shipping rules only.
