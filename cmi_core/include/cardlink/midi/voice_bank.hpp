#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace cardlink::midi
{

/** SAMPLE firmware: 8 dry voices (n0..n7). */
inline constexpr uint8_t kVoiceCount = 8;

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
  uint8_t velocity = 0;
  uint8_t active_count = 0;
};

struct VoiceSlot
{
  bool active = false;
  uint8_t midi_key = 0;
  uint8_t velocity = 0;
};

/**
 * 8-slot FIFO note bank (SAMPLE path).
 * Duplicate Note On retriggers the same slot; overflow steals the oldest.
 */
class VoiceBank
{
public:
  /** Set the playable slot count (1..8). Voices outside the new limit are
   * released and returned as Off events so callers can silence hardware. */
  std::vector<BankEvent> SetVoiceLimit(uint8_t voices);
  uint8_t VoiceLimit() const { return voice_limit_; }

  /** Apply Note On for key/velocity. Returns events (steal then on). */
  std::vector<BankEvent> NoteOn(uint8_t midi_key, uint8_t velocity = 127u);

  /** Apply Note Off for key. Empty if key was not active. */
  std::vector<BankEvent> NoteOff(uint8_t midi_key);

  /** Turn every active slot off (MIDI All Notes Off / clean shutdown). */
  std::vector<BankEvent> AllOff();

  /**
   * Force one slot from console `nX <hz>` (hz≤0 clears). Updates FIFO and
   * nearest-MIDI label for Perform; does not allocate by key.
   */
  std::vector<BankEvent> SetSlotKey(uint8_t slot, uint8_t midi_key,
                                    uint8_t velocity = 127u);
  std::vector<BankEvent> ClearSlot(uint8_t slot);

  /** Force all kVoiceCount slots to one raw key, or clear them. */
  std::vector<BankEvent> SetAllKey(uint8_t midi_key,
                                   uint8_t velocity = 127u);

  uint8_t ActiveCount() const;
  const std::array<VoiceSlot, kVoiceCount>& Slots() const { return slots_; }

private:
  std::optional<uint8_t> FindSlotByKey(uint8_t midi_key) const;
  std::optional<uint8_t> FindFreeSlot() const;
  void RemoveFromFifo(uint8_t slot);
  BankEvent MakeEvent(BankEventKind kind, uint8_t slot) const;

  std::array<VoiceSlot, kVoiceCount> slots_{};
  uint8_t voice_limit_ = kVoiceCount;
  /** Slot indices oldest → newest. */
  std::vector<uint8_t> fifo_;
};

} // namespace cardlink::midi
