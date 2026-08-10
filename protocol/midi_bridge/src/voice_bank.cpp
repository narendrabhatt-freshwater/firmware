#include "voice_bank.hpp"

#include "pitch.hpp"

#include <algorithm>

namespace midi_host
{

uint8_t VoiceBank::ActiveCount() const
{
  return static_cast<uint8_t>(fifo_.size());
}

BankEvent VoiceBank::MakeEvent(BankEventKind kind, uint8_t slot) const
{
  BankEvent ev;
  ev.kind = kind;
  ev.slot = slot;
  ev.midi_key = slots_[slot].midi_key;
  ev.freq_hz = slots_[slot].freq_hz;
  ev.active_count = ActiveCount();
  return ev;
}

std::optional<uint8_t> VoiceBank::FindSlotByKey(uint8_t midi_key) const
{
  for (uint8_t i = 0; i < kVoiceCount; ++i) {
    if (slots_[i].active && slots_[i].midi_key == midi_key) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<uint8_t> VoiceBank::FindFreeSlot() const
{
  for (uint8_t i = 0; i < kVoiceCount; ++i) {
    if (!slots_[i].active) {
      return i;
    }
  }
  return std::nullopt;
}

void VoiceBank::RemoveFromFifo(uint8_t slot)
{
  fifo_.erase(std::remove(fifo_.begin(), fifo_.end(), slot), fifo_.end());
}

std::vector<BankEvent> VoiceBank::NoteOn(uint8_t midi_key)
{
  std::vector<BankEvent> events;
  const double freq = MidiNoteToHz(midi_key);

  if (auto existing = FindSlotByKey(midi_key)) {
    const uint8_t slot = *existing;
    RemoveFromFifo(slot);
    slots_[slot].active = true;
    slots_[slot].midi_key = midi_key;
    slots_[slot].freq_hz = freq;
    fifo_.push_back(slot);
    events.push_back(MakeEvent(BankEventKind::Retrig, slot));
    return events;
  }

  if (auto free = FindFreeSlot()) {
    const uint8_t slot = *free;
    slots_[slot].active = true;
    slots_[slot].midi_key = midi_key;
    slots_[slot].freq_hz = freq;
    fifo_.push_back(slot);
    events.push_back(MakeEvent(BankEventKind::On, slot));
    return events;
  }

  // FIFO steal: oldest is front of fifo_
  const uint8_t steal_slot = fifo_.front();
  fifo_.erase(fifo_.begin());
  BankEvent steal = MakeEvent(BankEventKind::Steal, steal_slot);
  // Steal reports the key being dropped before we overwrite the slot.
  events.push_back(steal);

  slots_[steal_slot].active = true;
  slots_[steal_slot].midi_key = midi_key;
  slots_[steal_slot].freq_hz = freq;
  fifo_.push_back(steal_slot);
  events.push_back(MakeEvent(BankEventKind::On, steal_slot));
  return events;
}

std::vector<BankEvent> VoiceBank::NoteOff(uint8_t midi_key)
{
  std::vector<BankEvent> events;
  auto existing = FindSlotByKey(midi_key);
  if (!existing) {
    return events;
  }

  const uint8_t slot = *existing;
  BankEvent off = MakeEvent(BankEventKind::Off, slot);
  slots_[slot].active = false;
  slots_[slot].midi_key = 0;
  slots_[slot].freq_hz = 0.0;
  RemoveFromFifo(slot);
  off.active_count = ActiveCount();
  events.push_back(off);
  return events;
}

std::vector<BankEvent> VoiceBank::AllOff()
{
  std::vector<BankEvent> events;
  std::vector<uint8_t> keys;
  keys.reserve(fifo_.size());
  for (uint8_t slot : fifo_) {
    if (slots_[slot].active) {
      keys.push_back(slots_[slot].midi_key);
    }
  }
  for (uint8_t key : keys) {
    auto more = NoteOff(key);
    events.insert(events.end(), more.begin(), more.end());
  }
  return events;
}

std::vector<BankEvent> VoiceBank::SetSlotFreq(uint8_t slot, double freq_hz)
{
  std::vector<BankEvent> events;
  if (slot >= kVoiceCount) {
    return events;
  }

  if (freq_hz <= 0.0) {
    if (!slots_[slot].active) {
      return events;
    }
    BankEvent off = MakeEvent(BankEventKind::Off, slot);
    slots_[slot].active = false;
    slots_[slot].midi_key = 0;
    slots_[slot].freq_hz = 0.0;
    RemoveFromFifo(slot);
    off.active_count = ActiveCount();
    events.push_back(off);
    return events;
  }

  const bool was_active = slots_[slot].active;
  slots_[slot].active = true;
  slots_[slot].freq_hz = freq_hz;
  slots_[slot].midi_key = HzToNearestMidi(freq_hz);
  RemoveFromFifo(slot);
  fifo_.push_back(slot);
  events.push_back(MakeEvent(
      was_active ? BankEventKind::Retrig : BankEventKind::On, slot));
  return events;
}

std::vector<BankEvent> VoiceBank::SetAllFreq(double freq_hz)
{
  std::vector<BankEvent> events;
  if (freq_hz <= 0.0) {
    return AllOff();
  }
  for (uint8_t i = 0; i < kVoiceCount; ++i) {
    auto more = SetSlotFreq(i, freq_hz);
    events.insert(events.end(), more.begin(), more.end());
  }
  return events;
}

} // namespace midi_host
