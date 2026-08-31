#include "cardproto/channel.hpp"

#include <cstdio>

namespace cardproto
{
  namespace
  {

    bool ValidShapeParam(double p) { return p >= 0.1 && p <= 0.9; }

  } // namespace

  std::string FormatPulse(double duty)
  {
    char cmd[32];
    std::snprintf(cmd, sizeof(cmd), "p %.9f", duty);
    return cmd;
  }

  std::string FormatTriangle(double asymmetry)
  {
    char cmd[32];
    std::snprintf(cmd, sizeof(cmd), "t %.9f", asymmetry);
    return cmd;
  }

  Result ChannelClient::Sine() { return Send("s"); }

  Result ChannelClient::Saw() { return Send("saw"); }

  Result ChannelClient::Pulse(double duty)
  {
    if (!ValidShapeParam(duty))
    {
      return Result::LocalErr("range", "pulse duty");
    }
    return Send(FormatPulse(duty));
  }

  Result ChannelClient::Triangle(double asymmetry)
  {
    if (!ValidShapeParam(asymmetry))
    {
      return Result::LocalErr("range", "triangle asym");
    }
    return Send(FormatTriangle(asymmetry));
  }

} // namespace cardproto
