#include "cardproto/channel.hpp"

#include <cstdio>
#include <cstring>

namespace cardproto
{
  namespace
  {

    bool ValidSlot(uint8_t slot) { return slot <= 7u; }

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

  std::string FormatNoteOn(uint8_t slot, uint8_t key)
  {
    char cmd[48];
    const char hex = NoteSlotHex(slot);
    std::snprintf(cmd, sizeof(cmd), "n%c on %u", hex,
                  static_cast<unsigned>(key));
    return cmd;
  }

  std::string FormatStreamNoteOn(uint8_t slot, uint8_t key, uint8_t session)
  {
    char cmd[64];
    const char hex = NoteSlotHex(slot);
    std::snprintf(cmd, sizeof(cmd), "n%c on %u @%u", hex,
                  static_cast<unsigned>(key), static_cast<unsigned>(session));
    return cmd;
  }

  Result ChannelClient::NoteDefaults() { return Send("n0"); }

  Result ChannelClient::NoteOn(uint8_t slot, uint8_t key)
  {
    if (!ValidSlot(slot) || key >= 128u)
    {
      return Result::LocalErr("range", "note slot/key");
    }
    return Send(FormatNoteOn(slot, key));
  }

  Result ChannelClient::NoteOff(uint8_t slot) { return ValidSlot(slot)?Send("n"+std::to_string(slot)+" off"):Result::LocalErr("range","note slot"); }

  Result ChannelClient::StreamNoteOn(uint8_t slot, uint8_t key,
                                     uint8_t session)
  {
    if (!ValidSlot(slot) || key >= 128u || session >= 255u)
    {
      return Result::LocalErr("range", "stream note slot/key/session");
    }
    return Send(FormatStreamNoteOn(slot, key, session));
  }

  Result ChannelClient::AllNotesOff() { return Send("n off"); }

  Result ChannelClient::QueryVoiceStatus() { return Send("vq"); }

  bool ParseVoiceQuery(const char *raw, VoiceQuery &out)
  {
    if (raw == nullptr) {
      return false;
    }
    const char *p = raw;
    /* RS485 replies are tagged `[C] ok:vq …`. Result::raw should already
     * be the body; accept a leftover tag so a missed strip cannot stall
     * the idle watch (vq poll then starves BODY). */
    if (p[0] == '[' && p[1] != '\0' && p[2] == ']' &&
        (p[1] == 'C' || p[1] == 'c' || p[1] == 'E' || p[1] == 'e')) {
      p += 3;
    }
    while (*p == ' ') {
      ++p;
    }
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
    unsigned free_samples[8] = {};
    unsigned last_pack_sequence = 0;
    const int n = std::sscanf(
        p, " %x %u %u %u %u %u %u %u %u %u %u", &mask, &best,
        &free_samples[0], &free_samples[1], &free_samples[2],
        &free_samples[3], &free_samples[4], &free_samples[5],
        &free_samples[6], &free_samples[7], &last_pack_sequence);
    if (n != 11 || mask > 0xffu || best > 0xffu ||
        last_pack_sequence > 0xffffu) {
      return false;
    }
    for (unsigned i = 0; i < 8; ++i) {
      if (free_samples[i] > 8160u) {
        return false;
      }
      out.free_samples[i] = static_cast<uint16_t>(free_samples[i]);
    }
    out.mask = static_cast<uint8_t>(mask);
    out.best = static_cast<uint8_t>(best);
    out.last_pack_sequence = static_cast<uint16_t>(last_pack_sequence);
    return true;
  }

} // namespace cardproto
