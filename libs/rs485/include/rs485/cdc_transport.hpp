/**
 * @file cdc_transport.hpp
 * @brief protocol::IConsoleTransport over USB CDC (bare body replies).
 */

#ifndef RS485_CDC_TRANSPORT_HPP
#define RS485_CDC_TRANSPORT_HPP

#include "protocol/transport.hpp"

#include "rs485/serial_port.hpp"

#include <cstdint>

namespace rs485 {

struct CdcTransportOptions {
  uint32_t reply_timeout_ms = 500;
};

class CdcTransport final : public protocol::IConsoleTransport {
public:
  explicit CdcTransport(SerialPort &port, CdcTransportOptions opts = {});

  protocol::Result Exchange(protocol::Target target,
                            const std::string &command) override;
  bool SendBlind(protocol::Target target,
                 const std::string &command) override;

  SerialPort &Port() { return port_; }

private:
  SerialPort &port_;
  CdcTransportOptions opts_;
};

} // namespace rs485

#endif /* RS485_CDC_TRANSPORT_HPP */
