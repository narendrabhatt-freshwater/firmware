/**
 * @file transport.hpp
 * @brief Injected send/receive boundary for typed clients (no serial here).
 *
 * ChannelClient / EffectClient never open ports. Host code implements
 * IConsoleTransport with UART, CDC, mock, or cardlink transport adapters.
 */

#ifndef PROTOCOL_TRANSPORT_HPP
#define PROTOCOL_TRANSPORT_HPP

#include "cardproto/result.hpp"

#include <string>

namespace cardproto {

/**
 * @brief Host pipe adapter used by typed console clients.
 *
 * Framing rules for implementations:
 * - @p command is the ASCII token list **without** a trailing CR.
 * - Append a single `\r` before TX. Do not append `\r\n` (CR and LF both
 *   end a line on the card → double execute).
 * - Optionally prepend TargetPrefix(@p target) for multi-drop RS485.
 * - Wait for one reply line ending `\r\n`, then return ParseReplyBody.
 *
 * SendBlind uses the same TX framing but does not wait for a reply. It is
 * only for stuck-bus recovery (e.g. blind `"n off"`).
 */
class IConsoleTransport {
public:
  virtual ~IConsoleTransport() = default;

  /**
   * @brief Send one command and wait for one reply.
   *
   * @param[in] target  Address for prefixing / reply expectation.
   * @param[in] command ASCII tokens without trailing `\r`.
   * @return ParseReplyBody of the reply, or Timeout / IoError from the pipe.
   */
  virtual Result Exchange(Target target, const std::string &command) = 0;

  /**
   * @brief Transmit one command without waiting for a reply.
   *
   * @param[in] target  Address for optional prefix.
   * @param[in] command ASCII tokens without trailing `\r`.
   * @return True if the bytes were written; false on I/O failure.
   */
  virtual bool SendBlind(Target target, const std::string &command) = 0;
};

} // namespace cardproto

#endif /* PROTOCOL_TRANSPORT_HPP */
