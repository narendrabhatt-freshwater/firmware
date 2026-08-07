/**
 * @file rs485_transport.hpp
 * @brief protocol::IConsoleTransport over rs485::Link.
 */

#ifndef RS485_TRANSPORT_ADAPTER_HPP
#define RS485_TRANSPORT_ADAPTER_HPP

#include "protocol/transport.hpp"

#include "rs485/link.hpp"

namespace rs485 {

class Rs485Transport final : public protocol::IConsoleTransport {
public:
  explicit Rs485Transport(Link &link);

  protocol::Result Exchange(protocol::Target target,
                            const std::string &command) override;
  bool SendBlind(protocol::Target target,
                 const std::string &command) override;

  Link &GetLink() { return link_; }
  const Link &GetLink() const { return link_; }

private:
  Link &link_;
};

} // namespace rs485

#endif /* RS485_TRANSPORT_ADAPTER_HPP */
