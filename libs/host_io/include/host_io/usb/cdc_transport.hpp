/**
 * @file cdc_transport.hpp
 * @brief protocol::IConsoleTransport over USB CDC (bare body replies).
 */

#ifndef HOST_IO_USB_CDC_TRANSPORT_HPP
#define HOST_IO_USB_CDC_TRANSPORT_HPP

#include "protocol/transport.hpp"

#include "host_io/serial_port.hpp"

#include <cstdint>

namespace host_io {
namespace usb {

struct CdcTransportOptions {
  uint32_t reply_timeout_ms = 500;
};

class CdcTransport final : public protocol::IConsoleTransport {
public:
  explicit CdcTransport(host_io::SerialPort &port, CdcTransportOptions opts = {});

  protocol::Result Exchange(protocol::Target target,
                            const std::string &command) override;
  bool SendBlind(protocol::Target target,
                 const std::string &command) override;

  host_io::SerialPort &Port() { return port_; }

private:
  host_io::SerialPort &port_;
  CdcTransportOptions opts_;
};

} // namespace usb
} // namespace host_io

#endif /* HOST_IO_USB_CDC_TRANSPORT_HPP */
