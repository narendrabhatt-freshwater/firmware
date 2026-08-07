#include "protocol/channel.hpp"

#include <cstdio>

namespace protocol
{
  namespace
  {

    bool ValidFiltSlot(uint8_t slot) { return slot <= 7u; }

    bool ValidCutoff(double hz)
    {
      return hz == 0.0 || (hz >= 20.0 && hz <= 20000.0);
    }

    bool ValidQ(double q) { return q >= 0.5 && q <= 10.0; }

    bool ValidFk(double k) { return k >= 0.0 && k <= 10.0; }

  } // namespace

  std::string FormatSetFilter(uint8_t slot, double hz, double q)
  {
    char cmd[48];
    if (q < 0.0)
    {
      std::snprintf(cmd, sizeof(cmd), "f%u %.9f", static_cast<unsigned>(slot),
                    hz);
    }
    else
    {
      std::snprintf(cmd, sizeof(cmd), "f%u %.9f %.9f",
                    static_cast<unsigned>(slot), hz, q);
    }
    return cmd;
  }

  std::string FormatSetFilters(double hz, double q)
  {
    char cmd[48];
    if (q < 0.0)
    {
      std::snprintf(cmd, sizeof(cmd), "f %.9f", hz);
    }
    else
    {
      std::snprintf(cmd, sizeof(cmd), "f %.9f %.9f", hz, q);
    }
    return cmd;
  }

  std::string FormatSetFk(uint8_t slot, double k)
  {
    char cmd[40];
    std::snprintf(cmd, sizeof(cmd), "fk%u %.9f", static_cast<unsigned>(slot), k);
    return cmd;
  }

  std::string FormatSetFkAll(double k)
  {
    char cmd[40];
    std::snprintf(cmd, sizeof(cmd), "fk %.9f", k);
    return cmd;
  }

  Result ChannelClient::GetFilters() { return Send("f"); }

  Result ChannelClient::SetFilters(double hz, double q)
  {
    if (!ValidCutoff(hz))
    {
      return Result::LocalErr("range", "f hz");
    }
    if (q >= 0.0 && !ValidQ(q))
    {
      return Result::LocalErr("range", "f q");
    }
    return Send(FormatSetFilters(hz, q));
  }

  Result ChannelClient::GetFilter(uint8_t slot)
  {
    if (!ValidFiltSlot(slot))
    {
      return Result::LocalErr("range", "f slot");
    }
    char cmd[8];
    std::snprintf(cmd, sizeof(cmd), "f%u", static_cast<unsigned>(slot));
    return Send(cmd);
  }

  Result ChannelClient::SetFilter(uint8_t slot, double hz, double q)
  {
    if (!ValidFiltSlot(slot) || !ValidCutoff(hz))
    {
      return Result::LocalErr("range", "f");
    }
    if (q >= 0.0 && !ValidQ(q))
    {
      return Result::LocalErr("range", "f q");
    }
    return Send(FormatSetFilter(slot, hz, q));
  }

  Result ChannelClient::GetFk() { return Send("fk"); }

  Result ChannelClient::SetFk(double k)
  {
    if (!ValidFk(k))
    {
      return Result::LocalErr("range", "fk");
    }
    return Send(FormatSetFkAll(k));
  }

  Result ChannelClient::GetFk(uint8_t slot)
  {
    if (!ValidFiltSlot(slot))
    {
      return Result::LocalErr("range", "fk slot");
    }
    char cmd[8];
    std::snprintf(cmd, sizeof(cmd), "fk%u", static_cast<unsigned>(slot));
    return Send(cmd);
  }

  Result ChannelClient::SetFk(uint8_t slot, double k)
  {
    if (!ValidFiltSlot(slot) || !ValidFk(k))
    {
      return Result::LocalErr("range", "fk");
    }
    return Send(FormatSetFk(slot, k));
  }

} // namespace protocol
