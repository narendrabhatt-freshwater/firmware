/**
 * @file transport.hpp
 * @brief protocol::IConsoleTransport over host_io::rs485::Link.
 */

#ifndef HOST_IO_RS485_TRANSPORT_HPP
#define HOST_IO_RS485_TRANSPORT_HPP

#include "protocol/transport.hpp"

#include "host_io/rs485/link.hpp"

namespace host_io {
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
} // namespace host_io

#endif /* HOST_IO_RS485_TRANSPORT_HPP */
