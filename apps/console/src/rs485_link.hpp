/**
 * @file rs485_link.hpp
 * @brief RS485 console wire protocol — mirrors the on-firmware parser.
 *
 * Counterpart to `Console_Poll()` / `RS485_IsForMe()` / `RS485_Reply()` in
 * apps/channel_card/Core/Src/main.c (and the identical logic in
 * apps/effect_card/Core/Src/main.c): CRLF-terminated lines, an optional
 * "c:"/"e:"/"*:" address prefix, and `[C] `/`[E] `-tagged replies. See
 * docs/rs485_console_architecture.md for the full picture.
 */

#ifndef RS485_CONSOLE_RS485_LINK_HPP
#define RS485_CONSOLE_RS485_LINK_HPP

#include "serial_port.hpp"

#include <cstdint>
#include <string>

namespace rs485 {

enum class Target { Channel, Effect, All };

/** "c:" / "e:" / "*:" — the wire prefix for each target. */
std::string TargetPrefix(Target t);

/** Human-friendly name ("channel"/"effect"/"all") for status output. */
std::string TargetName(Target t);

/** Parses "channel"/"c"/"effect"/"e"/"all"/"*" (case-insensitive).
 * Returns false (leaves `out` unchanged) if `s` doesn't match any of
 * them. */
bool ParseTarget(const std::string &s, Target &out);

struct ExchangeResult {
  /** False if no reply arrived within the read window after all retries
   * — the bus is busy or no card is listening, not necessarily an error
   * (mirrors RS485_WaitBusFree()'s "drop, don't fail" behavior on the
   * firmware side). */
  bool got_reply = false;
  std::string reply; /**< Raw bytes received, CRLF as sent by the card(s). */
};

struct LinkOptions {
  /** Per-attempt read window before deciding "no reply yet". Mirrors
   * RS485_BUS_TIMEOUT_MS (250 ms) on the firmware side with a little
   * headroom for USB-serial adapter latency. */
  uint32_t reply_timeout_ms = 300;
  /** Once data starts arriving, stop reading after this long without a
   * new byte — lets multi-line replies (e.g. `status`) finish. */
  uint32_t idle_gap_ms = 80;
  /** Extra send attempts if no reply arrives at all (shared, collision
   * -avoided bus — a single timeout means "busy", not "gone"). */
  int retries = 2;
};

/** One request/response exchange over a shared RS485 bus. Stateless
 * across calls other than the timing knobs in LinkOptions. */
class RS485Link {
public:
  explicit RS485Link(SerialPort &port, LinkOptions opts = {});

  /** Sends `command` to `target`. If `command` already starts with an
   * explicit "c:"/"e:"/"*:" prefix (mirrors RS485_IsForMe()'s "X:" check),
   * that prefix is used as-is and `target` is ignored for this call. */
  ExchangeResult Send(Target target, const std::string &command);

private:
  SerialPort &port_;
  LinkOptions opts_;

  std::string ReadReply();
};

} // namespace rs485

#endif // RS485_CONSOLE_RS485_LINK_HPP
