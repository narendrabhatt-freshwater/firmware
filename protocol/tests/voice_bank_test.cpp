#include "cardlink/midi/voice_bank.hpp"

#include <cstdlib>
#include <iostream>

namespace
{

void Check(bool condition, const char *message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

} // namespace

int main()
{
  using cardlink::midi::BankEventKind;
  using cardlink::midi::VoiceBank;

  VoiceBank bank;
  for (uint8_t key = 60; key < 68; ++key) {
    const auto events = bank.NoteOn(key);
    Check(events.size() == 1 && events[0].kind == BankEventKind::On,
          "initial notes must allocate free voices");
  }
  Check(bank.ActiveCount() == 8, "all voices must be active");

  const auto retrigger = bank.NoteOn(60);
  Check(retrigger.size() == 1 &&
            retrigger[0].kind == BankEventKind::Retrig &&
            retrigger[0].slot == 0,
        "duplicate note must retrigger its existing slot");

  const auto overflow = bank.NoteOn(68);
  Check(overflow.size() == 2 &&
            overflow[0].kind == BankEventKind::Steal &&
            overflow[0].midi_key == 61 &&
            overflow[1].kind == BankEventKind::On &&
            overflow[1].midi_key == 68,
        "overflow must steal the oldest FIFO voice");

  const auto off = bank.NoteOff(60);
  Check(off.size() == 1 && off[0].kind == BankEventKind::Off &&
            bank.ActiveCount() == 7 && !bank.Slots()[0].active,
        "note off must release the matching voice");
  Check(bank.NoteOff(42).empty(), "unknown note off must be ignored");

  VoiceBank mono;
  Check(mono.SetVoiceLimit(1).empty() && mono.VoiceLimit() == 1,
        "voice limit must be configurable");
  Check(mono.NoteOn(60).front().slot == 0, "mono must allocate slot zero");
  const auto mono_steal = mono.NoteOn(64);
  Check(mono_steal.size() == 2 &&
            mono_steal[0].kind == BankEventKind::Steal &&
            mono_steal[1].kind == BankEventKind::On &&
            mono_steal[1].slot == 0,
        "mono overflow must crash into slot zero");

  VoiceBank reduced;
  for (uint8_t key = 60; key < 64; ++key) {
    (void)reduced.NoteOn(key);
  }
  const auto limit_offs = reduced.SetVoiceLimit(2);
  Check(limit_offs.size() == 2 && reduced.ActiveCount() == 2,
        "lowering voice limit must release out-of-range slots");

  return EXIT_SUCCESS;
}
