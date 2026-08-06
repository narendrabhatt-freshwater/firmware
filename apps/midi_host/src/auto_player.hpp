#pragma once

#include "voice_bank.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

namespace midi_host
{

/**
 * Fixed looping Am demo (broken chords + melody) for --auto mode.
 * Schedules NoteOn/NoteOff through VoiceBank on a 16th-note grid.
 * Stop() only freezes the sequencer; caller must bank.AllOff() on handoff.
 */
class AutoPlayer
{
public:
  using Clock = std::chrono::steady_clock;

  void Start(Clock::time_point now);
  void Stop();
  bool Running() const { return running_; }

  /** Advance the grid; append VoiceBank events for any note starts/ends. */
  std::vector<BankEvent> Tick(VoiceBank& bank, Clock::time_point now);

private:
  struct ActiveNote
  {
    uint8_t key = 0;
    int64_t off_tick = 0;
  };

  void FireTick(VoiceBank& bank, int64_t abs_tick, std::vector<BankEvent>& out);
  void NoteOffKey(VoiceBank& bank, uint8_t key, std::vector<BankEvent>& out);

  bool running_ = false;
  Clock::time_point start_{};
  int64_t last_tick_ = -1;
  std::vector<ActiveNote> active_;
};

} // namespace midi_host
