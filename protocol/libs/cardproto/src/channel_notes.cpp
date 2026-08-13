#include "cardproto/channel.hpp"

#include <cstdio>
#include <cstring>

namespace cardproto
{
  namespace
  {

    bool ValidSlot(uint8_t slot) { return slot <= 7u; }

    bool ValidHzOrOff(double hz)
    {
      return hz == 0.0 || (hz >= 20.0 && hz < 20000.0);
    }

    bool ValidScale(double scale) { return scale >= 0.0 && scale <= 1.0; }

  } // namespace

  ChannelClient::ChannelClient(IConsoleTransport &transport) : tx_(transport) {}

  Result ChannelClient::Send(const std::string &cmd)
  {
    return tx_.Exchange(Target::Channel, cmd);
  }

  Result ChannelClient::Help() { return Send("h"); }

  Result ChannelClient::Exec(const std::string &command)
  {
    return Send(command);
  }

  static char NoteSlotHex(uint8_t slot)
  {
    if (slot < 10u)
    {
      return static_cast<char>('0' + slot);
    }
    return static_cast<char>('a' + (slot - 10u));
  }

  std::string FormatSetNote(uint8_t slot, double hz, double scale)
  {
    char cmd[48];
    const char hex = NoteSlotHex(slot);
    if (hz == 0.0)
    {
      std::snprintf(cmd, sizeof(cmd), "n%c 0", hex);
    }
    else if (scale < 0.0)
    {
      std::snprintf(cmd, sizeof(cmd), "n%c %.9f", hex, hz);
    }
    else
    {
      std::snprintf(cmd, sizeof(cmd), "n%c %.9f %.9f", hex, hz, scale);
    }
    return cmd;
  }

  std::string FormatSetAllNotes(double hz, double scale)
  {
    char cmd[48];
    if (hz == 0.0)
    {
      std::snprintf(cmd, sizeof(cmd), "n 0");
    }
    else if (scale < 0.0)
    {
      std::snprintf(cmd, sizeof(cmd), "n %.9f", hz);
    }
    else
    {
      std::snprintf(cmd, sizeof(cmd), "n %.9f %.9f", hz, scale);
    }
    return cmd;
  }

  Result ChannelClient::NoteDefaults() { return Send("n0"); }

  Result ChannelClient::SetNote(uint8_t slot, double hz, double scale)
  {
    if (!ValidSlot(slot) || !ValidHzOrOff(hz))
    {
      return Result::LocalErr("range", "note slot/hz");
    }
    if (scale >= 0.0 && !ValidScale(scale))
    {
      return Result::LocalErr("range", "scale");
    }
    return Send(FormatSetNote(slot, hz, scale));
  }

  Result ChannelClient::NoteOff(uint8_t slot) { return SetNote(slot, 0.0); }

  Result ChannelClient::SetAllNotes(double hz, double scale)
  {
    if (!ValidHzOrOff(hz))
    {
      return Result::LocalErr("range", "hz");
    }
    if (scale >= 0.0 && !ValidScale(scale))
    {
      return Result::LocalErr("range", "scale");
    }
    return Send(FormatSetAllNotes(hz, scale));
  }

  Result ChannelClient::AllNotesOff() { return Send("n 0"); }

  Result ChannelClient::QueryVoiceStatus() { return Send("vq"); }

  bool ParseVoiceQuery(const char *raw, VoiceQuery &out)
  {
    if (raw == nullptr) {
      return false;
    }
    const char *p = raw;
    if (std::strncmp(p, "ok:", 3) == 0) {
      p += 3;
    }
    while (*p == ' ') {
      ++p;
    }
    if (std::strncmp(p, "vq", 2) == 0) {
      p += 2;
    }
    unsigned mask = 0;
    unsigned best = 0;
    unsigned s[8] = {};
    const int n = std::sscanf(p, " %x %u %u %u %u %u %u %u %u %u", &mask, &best,
                              &s[0], &s[1], &s[2], &s[3], &s[4], &s[5], &s[6],
                              &s[7]);
    if (n != 10 || mask > 0xffu || best > 0xffu) {
      return false;
    }
    for (unsigned i = 0; i < 8; ++i) {
      if (s[i] > 8u) {
        return false;
      }
      out.free_slots[i] = static_cast<uint8_t>(s[i]);
    }
    out.mask = static_cast<uint8_t>(mask);
    out.best = static_cast<uint8_t>(best);
    return true;
  }

} // namespace cardproto
