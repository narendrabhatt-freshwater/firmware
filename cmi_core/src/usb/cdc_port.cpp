#include "cardlink/usb/cdc_port.hpp"

#include <chrono>
#include <thread>

namespace cardlink {
namespace usb {
namespace {

void DrainRx(cardlink::SerialPort &port)
{
  uint8_t buf[256];
  for (uint8_t attempt = 0; attempt < 8u; ++attempt) {
    if (port.ReadTimeout(buf, sizeof(buf), 5) == 0) {
      break;
    }
  }
}

} // namespace

bool LooksLikeRs485AdapterPath(const std::string &path)
{
  return path.find("usbserial") != std::string::npos ||
         path.find("SLAB_USBtoUART") != std::string::npos ||
         path.find("usb-serial") != std::string::npos ||
         path.find("ttyUSB") != std::string::npos;
}

bool OpenCdcPort(cardlink::SerialPort &port,
                 const std::string &cdc_path,
                 std::string &err_out,
                 uint32_t baud)
{
  port.Close();
  if (cdc_path.empty()) {
    err_out = "err: no Channel Card USB CDC path";
    return false;
  }
  if (LooksLikeRs485AdapterPath(cdc_path)) {
    err_out = "err: that looks like the RS485 adapter — select the Channel "
              "Card USB CDC port for sample upload";
    return false;
  }
  if (!port.Open(cdc_path, baud)) {
    err_out = "err: Channel Card CDC open " + port.LastError();
    return false;
  }
  port.SetDtr(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  DrainRx(port);
  port.FlushInput();
  return true;
}

} // namespace usb
} // namespace cardlink
