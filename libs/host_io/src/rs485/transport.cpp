#include "host_io/rs485/transport.hpp"

#include <cstdio>

namespace host_io {
namespace rs485 {
namespace {

Target ToRs485(protocol::Target t)
{
  switch (t) {
  case protocol::Target::Channel:
    return Target::Channel;
  case protocol::Target::Effect:
    return Target::Effect;
  case protocol::Target::All:
    return Target::All;
  }
  return Target::All;
}

protocol::Result FromExchange(const ExchangeResult &ex)
{
  protocol::Result r;
  r.status = static_cast<protocol::Status>(ex.status);
  r.from = static_cast<protocol::Target>(ex.from);
  std::snprintf(r.err_code, sizeof(r.err_code), "%s", ex.err_code);
  std::snprintf(r.raw, sizeof(r.raw), "%s", ex.raw);
  return r;
}

} // namespace

Rs485Transport::Rs485Transport(Link &link) : link_(link) {}

protocol::Result Rs485Transport::Exchange(protocol::Target target,
                                          const std::string &command)
{
  return FromExchange(link_.Send(ToRs485(target), command));
}

bool Rs485Transport::SendBlind(protocol::Target target,
                               const std::string &command)
{
  return link_.SendBlind(ToRs485(target), command);
}

} // namespace rs485
} // namespace host_io
