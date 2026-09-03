/**
 * @file cdc_transport.hpp
 * @brief cardproto::IConsoleTransport over USB CDC (bare body replies).
 */

#ifndef CARDLINK_USB_CDC_TRANSPORT_HPP
#define CARDLINK_USB_CDC_TRANSPORT_HPP

#include "cardproto/transport.hpp"

#include "cardlink/serial_port.hpp"

#include <cstdint>

namespace cardlink {
namespace usb {

struct CdcTransportOptions {
  uint32_t reply_timeout_ms = 500;
};

class CdcTransport final : public cardproto::IConsoleTransport {
public:
  explicit CdcTransport(cardlink::SerialPort &port, CdcTransportOptions opts = {});

  cardproto::Result Exchange(cardproto::Target target,
                            const std::string &command) override;
  bool SendBlind(cardproto::Target target,
                 const std::string &command) override;

  cardlink::SerialPort &Port() { return port_; }

private:
  cardlink::SerialPort &port_;
  CdcTransportOptions opts_;
};

} // namespace usb
} // namespace cardlink

#endif /* CARDLINK_USB_CDC_TRANSPORT_HPP */
