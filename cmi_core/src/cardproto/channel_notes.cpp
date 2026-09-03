#include "cardproto/channel.hpp"

#include <cstdio>
#include <cstring>
#include <sstream>

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

  std::string FormatNoteOn(uint8_t slot, uint8_t key, uint8_t velocity)
  {
    char cmd[48];
    const char hex = NoteSlotHex(slot);
    std::snprintf(cmd, sizeof(cmd), "n%c on %u %u", hex,
                  static_cast<unsigned>(key),
                  static_cast<unsigned>(velocity));
    return cmd;
  }

  std::string FormatStreamNoteOn(uint8_t slot, uint8_t key, uint8_t velocity,
                                 uint8_t session)
  {
    char cmd[64];
    const char hex = NoteSlotHex(slot);
    std::snprintf(cmd, sizeof(cmd), "n%c on %u %u @%u", hex,
                  static_cast<unsigned>(key),
                  static_cast<unsigned>(velocity),
                  static_cast<unsigned>(session));
    return cmd;
  }

  std::string FormatNoteOff(uint8_t slot)
  {
    return std::string("n") + NoteSlotHex(slot) + " off";
  }

  Result ChannelClient::NoteOn(uint8_t slot, uint8_t key, uint8_t velocity)
  {
    if (!ValidSlot(slot) || key >= 128u || velocity == 0u || velocity >= 128u)
    {
      return Result::LocalErr("range", "note slot/key/velocity");
    }
    return Send(FormatNoteOn(slot, key, velocity));
  }

  Result ChannelClient::NoteOff(uint8_t slot) { return ValidSlot(slot)?Send(FormatNoteOff(slot)):Result::LocalErr("range","note slot"); }

  Result ChannelClient::StreamNoteOn(uint8_t slot, uint8_t key,
                                     uint8_t velocity, uint8_t session)
  {
    if (!ValidSlot(slot) || key >= 128u || velocity == 0u ||
        velocity >= 128u || session >= 255u)
    {
      return Result::LocalErr("range",
                              "stream note slot/key/velocity/session");
    }
    return Send(FormatStreamNoteOn(slot, key, velocity, session));
  }

  Result ChannelClient::StreamNoteOn(uint8_t slot, uint8_t key,
                                     uint8_t session)
  {
    return StreamNoteOn(slot, key, 127u, session);
  }

  std::string FormatStreamNoteOn(uint8_t slot, uint8_t key, uint8_t session)
  {
    return FormatStreamNoteOn(slot, key, 127u, session);
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
    if (std::strncmp(p, "vq7", 3) != 0) {
      return false;
    }
    p += 3;
    std::istringstream in(p);
    unsigned active = 0, pending = 0, best = 0, capacity = 0, sequence = 0;
    unsigned uac_sequence = 0;
    in >> std::hex >> active >> pending >> std::dec >> best >> capacity >>
        sequence >> uac_sequence;
    if (!in || active > 0xffu || pending > 0xffu || best > 0xffu ||
        capacity == 0u || capacity > 0xffffu || sequence > 0xffffu ||
        uac_sequence > 0xffffu) {
      return false;
    }
    for (unsigned i = 0; i < 8; ++i) {
      unsigned session = 0, fill = 0, free_samples = 0;
      in >> session >> fill >> free_samples;
      if (!in || session > 0xffu || fill > capacity ||
          free_samples > capacity || fill + free_samples > capacity) {
        return false;
      }
      out.target_session[i] = static_cast<uint8_t>(session);
      out.target_fill[i] = static_cast<uint16_t>(fill);
      out.free_samples[i] = static_cast<uint16_t>(free_samples);
    }
    out.active_mask = static_cast<uint8_t>(active);
    out.pending_mask = static_cast<uint8_t>(pending);
    out.best = static_cast<uint8_t>(best);
    out.capacity = static_cast<uint16_t>(capacity);
    out.status_sequence = static_cast<uint16_t>(sequence);
    out.uac_sequence = static_cast<uint16_t>(uac_sequence);
    return true;
  }

} // namespace cardproto
