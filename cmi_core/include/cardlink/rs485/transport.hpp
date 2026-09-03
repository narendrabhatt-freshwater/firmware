/**
 * @file transport.hpp
 * @brief cardproto::IConsoleTransport over cardlink::rs485::Link.
 */

#ifndef CARDLINK_RS485_TRANSPORT_HPP
#define CARDLINK_RS485_TRANSPORT_HPP

#include "cardproto/transport.hpp"

#include "cardlink/rs485/link.hpp"

namespace cardlink {
namespace rs485 {

class Rs485Transport final : public cardproto::IConsoleTransport {
public:
  explicit Rs485Transport(Link &link);

  cardproto::Result Exchange(cardproto::Target target,
                            const std::string &command) override;
  bool SendBlind(cardproto::Target target,
                 const std::string &command) override;

  Link &GetLink() { return link_; }
  const Link &GetLink() const { return link_; }

private:
  Link &link_;
};

} // namespace rs485
} // namespace cardlink

#endif /* CARDLINK_RS485_TRANSPORT_HPP */
