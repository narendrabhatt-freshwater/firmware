#include "protocol/channel.hpp"

#include <cstdio>

namespace protocol
{

  std::string FormatMode(PlayMode mode)
  {
    return (mode == PlayMode::Wave) ? "m 1" : "m 0";
  }

  Result ChannelClient::GetMode() { return Send("m"); }

  Result ChannelClient::SetMode(PlayMode mode)
  {
    return Send(FormatMode(mode));
  }

} // namespace protocol
