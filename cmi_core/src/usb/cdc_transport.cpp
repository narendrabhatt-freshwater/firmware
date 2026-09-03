#include "cardlink/usb/cdc_transport.hpp"

#include "cardproto/types.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace cardlink {
namespace usb {
namespace {

std::string WireCommand(cardproto::Target target, const std::string &command)
{
  if (command.size() >= 2 && command[1] == ':') {
    return command;
  }
  return cardproto::TargetPrefix(target) + command;
}

bool WaitLine(cardlink::SerialPort &port, std::string &accum, uint32_t timeout_ms)
{
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  uint8_t buf[256];
  while (std::chrono::steady_clock::now() < deadline) {
    const size_t n = port.ReadTimeout(buf, sizeof(buf), 20);
    if (n == 0) {
      continue;
    }
    accum.append(reinterpret_cast<const char *>(buf), n);
    if (accum.find("\r\n") != std::string::npos ||
        accum.find('\n') != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string FirstLine(const std::string &accum)
{
  size_t end = accum.find("\r\n");
  if (end != std::string::npos) {
    return accum.substr(0, end);
  }
  end = accum.find('\n');
  if (end != std::string::npos) {
    return accum.substr(0, end);
  }
  end = accum.find('\r');
  if (end != std::string::npos) {
    return accum.substr(0, end);
  }
  return accum;
}

} // namespace

CdcTransport::CdcTransport(cardlink::SerialPort &port, CdcTransportOptions opts)
    : port_(port), opts_(opts)
{
}

cardproto::Result CdcTransport::Exchange(cardproto::Target target,
                                        const std::string &command)
{
  if (!port_.IsOpen()) {
    return cardproto::Result::IoErr("cdc not open");
  }

  const std::string wire = WireCommand(target, command) + "\r";
  if (!port_.Write(reinterpret_cast<const uint8_t *>(wire.data()),
                   wire.size())) {
    return cardproto::Result::IoErr(port_.LastError().c_str());
  }
  port_.DrainOutput();

  std::string accum;
  if (!WaitLine(port_, accum, opts_.reply_timeout_ms)) {
    cardproto::Result r;
    r.status = cardproto::Status::Timeout;
    std::snprintf(r.raw, sizeof(r.raw), "%s",
                  accum.empty() ? "(empty)" : accum.c_str());
    return r;
  }
  return cardproto::ParseReplyBody(FirstLine(accum), target);
}

bool CdcTransport::SendBlind(cardproto::Target target,
                             const std::string &command)
{
  if (!port_.IsOpen()) {
    return false;
  }
  const std::string wire = WireCommand(target, command) + "\r";
  const bool ok = port_.Write(reinterpret_cast<const uint8_t *>(wire.data()),
                              wire.size());
  if (ok) {
    port_.DrainOutput();
  }
  return ok;
}

} // namespace usb
} // namespace cardlink
