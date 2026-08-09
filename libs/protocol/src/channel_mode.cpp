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
    Result r = Send(FormatMode(mode));
    /* Pre-shortening firmware only understood "mode notes|wave". */
    if (!r.ok() && r.status == Status::Err)
    {
      const char *legacy =
          (mode == PlayMode::Wave) ? "mode wave" : "mode notes";
      r = Send(legacy);
    }
    return r;
  }

} // namespace protocol
