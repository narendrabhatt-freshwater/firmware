# cardproto — Freshwater console wire API

C++17 library for the Channel / Effect Card **ASCII command language**.
Normative wire contract: the Freshwater console protocol specification
(`protocol.md`, shipped alongside this library or kept in the product docs).

This package has **no serial / RS485 / USB I/O**. It only:

1. Builds command strings (`Format*`)
2. Parses reply bodies (`ParseReplyBody`)
3. Offers typed clients (`ChannelClient` / `EffectClient`) that call an
   injected `IConsoleTransport`

Ship this library with that protocol specification to a host team that
already owns a UART, CDC, or other pipe.

Optional Freshwater PC glue (ports, tagged RS485 link, CDC sample upload):
`protocol/libs/cardlink` in the product tree. Not required to use this library.

---

## Build

```cmake
add_subdirectory(path/to/protocol/libs/cardproto)
target_link_libraries(your_app PRIVATE cardproto::cardproto)
```

C++17. Header root: `protocol/libs/cardproto/include` → `#include "cardproto/…"`.

---

## Framing (host responsibility)

| Direction | Rule |
| --------- | ---- |
| Host → card | One ASCII command, **no** trailing `\r` in `Format*` / `Exchange` `command`. The transport appends a single `\r`. Do not send `\r\n` (both CR and LF end a line → double execute). |
| Card → host | One reply ending `\r\n`. RS485 bodies are often tagged `[C] ` / `[E] `; USB CDC is usually bare. `ParseReplyBody` accepts both. |
| Addressing | `TargetPrefix(Channel)` → `c:`, Effect → `e:`, All → `*:`. Prefix is optional on a dedicated CDC link; useful on multi-drop RS485. |

Input on the card is folded to lower case. Success bodies start with `ok` /
`ok:…`; failures with `err:…`.

---

## Two usage styles

### 1. Format only

```cpp
#include "cardproto/channel.hpp"

std::string cmd = cardproto::FormatSetNote(0, 261.625565);
// Transport must send: cmd + '\r'
// Prefer fractional Hz — integer rounding detunes equal-temperament octaves.
```

`Format*` functions do **not** validate ranges (they only encode). Validation
lives in `ChannelClient` / `EffectClient`, which return `Result::LocalErr`
without calling the transport when args are out of range.

### 2. Typed client + your transport

```cpp
#include "cardproto/channel.hpp"
#include "cardproto/transport.hpp"

struct MyTransport : cardproto::IConsoleTransport {
  cardproto::Result Exchange(cardproto::Target t,
                             const std::string &command) override {
    // 1) Optionally prepend cardproto::TargetPrefix(t)
    // 2) Write command + '\r'
    // 3) Read one reply line
    // 4) return cardproto::ParseReplyBody(line, t);
  }
  bool SendBlind(cardproto::Target t, const std::string &command) override {
    // Write command + '\r'; do not wait (bus recovery only)
    return true;
  }
};

MyTransport tx;
cardproto::ChannelClient ch(tx);
cardproto::Result r = ch.SetNote(0, 440.0);
if (!r.ok()) { /* r.status, r.err_code, r.raw */ }
```

`ChannelClient` always exchanges with `Target::Channel`; `EffectClient` with
`Target::Effect`. The transport may still add `c:` / `e:` for a shared bus.

---

## Error model

| `Status` | Meaning |
| -------- | ------- |
| `Ok` | Body is `ok` or `ok:…` |
| `Err` | Card or local reject; see `err_code` / `raw` |
| `Timeout` | No terminal reply in time (transport) |
| `IoError` | Port / write failure (transport) |
| `BadReply` | Line present but not `ok`/`err:` |

Local validation failures use `Result::LocalErr("range", …)` or
`("syntax", …)` and **do not** hit the bus.

Binary sample **upload** (`al` / `bl` + payload) is not in this library — it
needs a byte stream. See the console protocol specification (USB CDC section)
and, in the product tree, `protocol/libs/cardlink`
(`cardlink::usb::AttackUploader` / `BodyUploader`).

---

## Headers

| Header | Contents |
| ------ | -------- |
| `cardproto/types.hpp` | `Target`, `Status`, `TargetPrefix` |
| `cardproto/result.hpp` | `Result`, `ParseReplyBody` |
| `cardproto/transport.hpp` | `IConsoleTransport` |
| `cardproto/channel.hpp` | Channel card API + `Format*` (notes, shape, env, filter, gain, cpu) |
| `cardproto/effect.hpp` | Effect card API + `Format*` |

`channel.hpp` still carries formatters for the removed firmware commands
(`m` mode, `w` wave play, `wl` upload) and a 16-voice cpu range; the card
rejects those today. Treat them as pending removal — do not build new code
on them.

Public headers use Doxygen (`@brief`, `@param`, `@return`, bounds, wire
tokens). When header and the console protocol specification disagree, trust
the firmware `h` menu / that specification, then update this library.
