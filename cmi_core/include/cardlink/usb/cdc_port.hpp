/**
 * @file cdc_port.hpp
 * @brief Channel Card USB CDC port helpers.
 */

#ifndef CARDLINK_USB_CDC_PORT_HPP
#define CARDLINK_USB_CDC_PORT_HPP

#include "cardlink/serial_port.hpp"

#include <cstdint>
#include <string>

namespace cardlink {
namespace usb {

/** Open a Channel Card CDC port with DTR, settle time, and input drain. */
bool OpenCdcPort(cardlink::SerialPort &port,
                 const std::string &cdc_path,
                 std::string &err_out,
                 uint32_t baud = 115200);

/** Return true for common USB-UART paths used by the RS485 adapter. */
bool LooksLikeRs485AdapterPath(const std::string &path);

} // namespace usb
} // namespace cardlink

#endif /* CARDLINK_USB_CDC_PORT_HPP */
