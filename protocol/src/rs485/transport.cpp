#include "cardlink/rs485/transport.hpp"

#include <cstdio>

namespace cardlink {
namespace rs485 {
namespace {

Target ToRs485(cardproto::Target t)
{
  switch (t) {
  case cardproto::Target::Channel:
    return Target::Channel;
  case cardproto::Target::Effect:
    return Target::Effect;
  case cardproto::Target::All:
    return Target::All;
  }
  return Target::All;
}

cardproto::Result FromExchange(const ExchangeResult &ex)
{
  cardproto::Result r;
  r.status = static_cast<cardproto::Status>(ex.status);
  r.from = static_cast<cardproto::Target>(ex.from);
  std::snprintf(r.err_code, sizeof(r.err_code), "%s", ex.err_code);
  std::snprintf(r.raw, sizeof(r.raw), "%s", ex.raw);
  return r;
}

} // namespace

Rs485Transport::Rs485Transport(Link &link) : link_(link) {}

cardproto::Result Rs485Transport::Exchange(cardproto::Target target,
                                          const std::string &command)
{
  return FromExchange(link_.Send(ToRs485(target), command));
}

bool Rs485Transport::SendBlind(cardproto::Target target,
                               const std::string &command)
{
  return link_.SendBlind(ToRs485(target), command);
}

} // namespace rs485
} // namespace cardlink
