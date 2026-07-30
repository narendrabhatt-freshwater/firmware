#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace midi_host
{

inline constexpr uint8_t kVoiceCount = 16;

enum class BankEventKind : uint8_t
{
  On = 0,
  Off = 1,
  Steal = 2,
  Retrig = 3,
};

struct BankEvent
{
  BankEventKind kind = BankEventKind::Off;
  uint8_t slot = 0;
  uint8_t midi_key = 0;
  double freq_hz = 0.0;
  uint8_t active_count = 0;
};

struct VoiceSlot
{
  bool active = false;
  uint8_t midi_key = 0;
  double freq_hz = 0.0;
};

/**
 * 16-slot FIFO note bank.
 * Duplicate Note On retriggers the same slot; overflow steals the oldest.
 */
class VoiceBank
{
public:
  /** Apply Note On for key. Returns one or more events (steal then on). */
  std::vector<BankEvent> NoteOn(uint8_t midi_key);

  /** Apply Note Off for key. Empty if key was not active. */
  std::vector<BankEvent> NoteOff(uint8_t midi_key);

  /** Turn every active slot off (MIDI All Notes Off / clean shutdown). */
  std::vector<BankEvent> AllOff();

  uint8_t ActiveCount() const;
  const std::array<VoiceSlot, kVoiceCount>& Slots() const { return slots_; }

private:
  std::optional<uint8_t> FindSlotByKey(uint8_t midi_key) const;
  std::optional<uint8_t> FindFreeSlot() const;
  void RemoveFromFifo(uint8_t slot);
  BankEvent MakeEvent(BankEventKind kind, uint8_t slot) const;

  std::array<VoiceSlot, kVoiceCount> slots_{};
  /** Slot indices oldest → newest. */
  std::vector<uint8_t> fifo_;
};

} // namespace midi_host
