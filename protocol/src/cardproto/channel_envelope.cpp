#include "cardproto/channel.hpp"

#include <cstdio>

namespace cardproto
{
  namespace
  {

    bool ValidSlot(uint8_t slot) { return slot <= 7u; }

    bool ValidK(double k) { return k >= -10.0 && k <= 10.0; }

    char Hex(uint8_t slot)
    {
      if (slot < 10u)
      {
        return static_cast<char>('0' + slot);
      }
      return static_cast<char>('a' + (slot - 10u));
    }

  } // namespace

  std::string FormatSetEnvelope(uint8_t slot, const std::string &tokens)
  {
    char head[8];
    std::snprintf(head, sizeof(head), "en%c ", Hex(slot));
    return std::string(head) + tokens;
  }

  std::string FormatSetEnvelopeAll(const std::string &tokens)
  {
    return std::string("en ") + tokens;
  }

  std::string FormatSetEnvK(uint8_t slot, double k)
  {
    char cmd[40];
    std::snprintf(cmd, sizeof(cmd), "ek%c %.9f", Hex(slot), k);
    return cmd;
  }

  std::string FormatSetEnvKAll(double k)
  {
    char cmd[40];
    std::snprintf(cmd, sizeof(cmd), "ek %.9f", k);
    return cmd;
  }

  Result ChannelClient::ListEnvelopes() { return Send("en"); }

  Result ChannelClient::SetEnvelopeAll(const std::string &tokens)
  {
    if (tokens.empty())
    {
      return Result::LocalErr("syntax", "empty en tokens");
    }
    return Send(FormatSetEnvelopeAll(tokens));
  }

  Result ChannelClient::ClearEnvelopeAll() { return Send("en 0"); }

  Result ChannelClient::GetEnvelope(uint8_t slot)
  {
    if (!ValidSlot(slot))
    {
      return Result::LocalErr("range", "en slot");
    }
    char cmd[8];
    std::snprintf(cmd, sizeof(cmd), "en%c", Hex(slot));
    return Send(cmd);
  }

  Result ChannelClient::SetEnvelope(uint8_t slot, const std::string &tokens)
  {
    if (!ValidSlot(slot) || tokens.empty())
    {
      return Result::LocalErr("range", "en");
    }
    return Send(FormatSetEnvelope(slot, tokens));
  }

  Result ChannelClient::ClearEnvelope(uint8_t slot)
  {
    if (!ValidSlot(slot))
    {
      return Result::LocalErr("range", "en slot");
    }
    char cmd[12];
    std::snprintf(cmd, sizeof(cmd), "en%c 0", Hex(slot));
    return Send(cmd);
  }

  Result ChannelClient::GetEnvK() { return Send("ek"); }

  Result ChannelClient::SetEnvK(double k)
  {
    if (!ValidK(k))
    {
      return Result::LocalErr("range", "ek");
    }
    return Send(FormatSetEnvKAll(k));
  }

  Result ChannelClient::GetEnvK(uint8_t slot)
  {
    if (!ValidSlot(slot))
    {
      return Result::LocalErr("range", "ek slot");
    }
    char cmd[8];
    std::snprintf(cmd, sizeof(cmd), "ek%c", Hex(slot));
    return Send(cmd);
  }

  Result ChannelClient::SetEnvK(uint8_t slot, double k)
  {
    if (!ValidSlot(slot) || !ValidK(k))
    {
      return Result::LocalErr("range", "ek");
    }
    return Send(FormatSetEnvK(slot, k));
  }

} // namespace cardproto
