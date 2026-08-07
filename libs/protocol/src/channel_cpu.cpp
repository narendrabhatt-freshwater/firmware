#include "protocol/channel.hpp"

#include <cstdio>

namespace protocol
{

  std::string FormatCpu(CpuProbe kind, uint8_t nvoices)
  {
    char cmd[24];
    switch (kind)
    {
    case CpuProbe::Off:
      return "cpu 0";
    case CpuProbe::Queue:
      if (nvoices == 0u)
      {
        return "cpu q";
      }
      std::snprintf(cmd, sizeof(cmd), "cpu q %u",
                    static_cast<unsigned>(nvoices));
      return cmd;
    case CpuProbe::On:
    default:
      if (nvoices == 0u)
      {
        return "cpu";
      }
      std::snprintf(cmd, sizeof(cmd), "cpu %u",
                    static_cast<unsigned>(nvoices));
      return cmd;
    }
  }

  Result ChannelClient::Cpu(CpuProbe kind, uint8_t nvoices)
  {
    if (nvoices > 16u)
    {
      return Result::LocalErr("range", "cpu voices");
    }
    if (kind != CpuProbe::Off && nvoices == 0u)
    {
      /* bare cpu / cpu q — firmware defaults */
    }
    else if (kind != CpuProbe::Off && (nvoices < 1u || nvoices > 16u))
    {
      return Result::LocalErr("range", "cpu voices");
    }
    return Send(FormatCpu(kind, nvoices));
  }

} // namespace protocol
