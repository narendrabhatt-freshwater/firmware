#include "protocol/channel.hpp"

#include <cstdio>

namespace protocol
{

  std::string FormatGain(uint8_t ch, uint8_t atten_db)
  {
    char cmd[32];
    std::snprintf(cmd, sizeof(cmd), "g %u %u", static_cast<unsigned>(ch),
                  static_cast<unsigned>(atten_db));
    return cmd;
  }

  Result ChannelClient::SetGain(uint8_t ch, uint8_t atten_db)
  {
    if (ch < 1u || ch > 4u || atten_db > 127u)
    {
      return Result::LocalErr("range", "gain");
    }
    return Send(FormatGain(ch, atten_db));
  }

} // namespace protocol
