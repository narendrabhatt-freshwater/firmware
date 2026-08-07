#include "protocol/channel.hpp"

#include <cstdio>

namespace protocol
{
  namespace
  {

    bool ValidWaveSlot(uint8_t slot) { return slot <= 7u; }

    bool ValidRate(double rate)
    {
      return rate == 0.0 || (rate >= 1.0 && rate <= 192000.0);
    }

  } // namespace

  std::string FormatPlayWave(uint8_t slot, double rate_hz)
  {
    char cmd[40];
    std::snprintf(cmd, sizeof(cmd), "w%u %.0f", static_cast<unsigned>(slot),
                  rate_hz);
    return cmd;
  }

  std::string FormatStopWave(uint8_t slot)
  {
    char cmd[16];
    std::snprintf(cmd, sizeof(cmd), "w%u 0", static_cast<unsigned>(slot));
    return cmd;
  }

  Result ChannelClient::QueryWaves() { return Send("w"); }

  Result ChannelClient::QueryWave(uint8_t slot)
  {
    if (!ValidWaveSlot(slot))
    {
      return Result::LocalErr("range", "wave slot");
    }
    char cmd[8];
    std::snprintf(cmd, sizeof(cmd), "w%u", static_cast<unsigned>(slot));
    return Send(cmd);
  }

  Result ChannelClient::PlayWave(uint8_t slot, double rate_hz)
  {
    if (!ValidWaveSlot(slot) || !ValidRate(rate_hz) || rate_hz == 0.0)
    {
      return Result::LocalErr("range", "wave play");
    }
    return Send(FormatPlayWave(slot, rate_hz));
  }

  Result ChannelClient::StopWave(uint8_t slot)
  {
    if (!ValidWaveSlot(slot))
    {
      return Result::LocalErr("range", "wave slot");
    }
    return Send(FormatStopWave(slot));
  }

} // namespace protocol
