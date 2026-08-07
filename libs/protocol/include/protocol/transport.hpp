/**
 * @file transport.hpp
 * @brief Injected send/receive boundary for typed clients (no serial here).
 */

#ifndef PROTOCOL_TRANSPORT_HPP
#define PROTOCOL_TRANSPORT_HPP

#include "protocol/result.hpp"

#include <string>

namespace protocol {

/**
 * Host pipe adapter. ChannelClient / EffectClient never touch OS serial APIs.
 *
 * Exchange(command): `command` is the ASCII token list **without** trailing
 * CR. Implementations should append a single '\r', optionally prepend
 * TargetPrefix(target), wait for one reply line, and return ParseReplyBody.
 *
 * SendBlind: same TX framing, no reply wait — used only for stuck-bus
 * recovery (e.g. blind "n 0").
 */
class IConsoleTransport {
public:
  virtual ~IConsoleTransport() = default;

  virtual Result Exchange(Target target, const std::string &command) = 0;
  virtual bool SendBlind(Target target, const std::string &command) = 0;
};

} // namespace protocol

#endif /* PROTOCOL_TRANSPORT_HPP */
