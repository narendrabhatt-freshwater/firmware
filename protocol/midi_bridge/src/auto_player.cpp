/**
 * @file auto_player.cpp
 * @brief Looping A-minor demo for midi_host --auto mode.
 *
 * 96 BPM, 16th-note grid. Eight bars of Am–F–C–G: left-hand broken chords
 * plus a simple A-minor-pentatonic melody. At most ~3 concurrent voices.
 */

#include "auto_player.hpp"

#include <algorithm>

namespace midi_host
{
namespace
{

/* 96 BPM → quarter = 625 ms → 16th = 156.25 ms. */
constexpr double kTickMs = 60000.0 / (96.0 * 4.0);
constexpr int64_t kLoopTicks = 128; /* 8 bars × 16 sixteenths */

struct ScoreNote
{
  uint16_t start;    /* tick within loop [0, kLoopTicks) */
  uint8_t key;       /* MIDI note */
  uint16_t duration; /* length in 16ths */
};

/*
 * Left hand: root–fifth–third–fifth arpeggios on Am F C G (2 bars each).
 * Right hand: A-minor pentatonic melody (A C D E G), mid register.
 */
constexpr ScoreNote kScore[] = {
    /* —— bars 1–2 Am (ticks 0..31) —— */
    {0, 57, 2},  {2, 64, 2},  {4, 60, 2},  {6, 64, 2},
    {8, 57, 2},  {10, 64, 2}, {12, 60, 2}, {14, 64, 2},
    {16, 57, 2}, {18, 64, 2}, {20, 60, 2}, {22, 64, 2},
    {24, 57, 2}, {26, 64, 2}, {28, 60, 2}, {30, 64, 2},
    {0, 69, 4},  {4, 72, 4},  {8, 74, 4},  {12, 72, 4},
    {16, 76, 4}, {20, 74, 4}, {24, 72, 4}, {28, 69, 4},

    /* —— bars 3–4 F (ticks 32..63) —— */
    {32, 53, 2}, {34, 60, 2}, {36, 57, 2}, {38, 60, 2},
    {40, 53, 2}, {42, 60, 2}, {44, 57, 2}, {46, 60, 2},
    {48, 53, 2}, {50, 60, 2}, {52, 57, 2}, {54, 60, 2},
    {56, 53, 2}, {58, 60, 2}, {60, 57, 2}, {62, 60, 2},
    {32, 72, 4}, {36, 69, 4}, {40, 67, 4}, {44, 69, 4},
    {48, 72, 4}, {52, 74, 4}, {56, 72, 4}, {60, 69, 4},

    /* —— bars 5–6 C (ticks 64..95) —— */
    {64, 48, 2}, {66, 55, 2}, {68, 52, 2}, {70, 55, 2},
    {72, 48, 2}, {74, 55, 2}, {76, 52, 2}, {78, 55, 2},
    {80, 48, 2}, {82, 55, 2}, {84, 52, 2}, {86, 55, 2},
    {88, 48, 2}, {90, 55, 2}, {92, 52, 2}, {94, 55, 2},
    {64, 67, 4}, {68, 69, 4}, {72, 72, 4}, {76, 74, 4},
    {80, 76, 4}, {84, 74, 4}, {88, 72, 4}, {92, 67, 4},

    /* —— bars 7–8 G (ticks 96..127) —— */
    {96, 55, 2},  {98, 62, 2},  {100, 59, 2}, {102, 62, 2},
    {104, 55, 2}, {106, 62, 2}, {108, 59, 2}, {110, 62, 2},
    {112, 55, 2}, {114, 62, 2}, {116, 59, 2}, {118, 62, 2},
    {120, 55, 2}, {122, 62, 2}, {124, 59, 2}, {126, 62, 2},
    {96, 74, 4},  {100, 72, 4}, {104, 71, 4}, {108, 69, 4},
    {112, 67, 4}, {116, 69, 4}, {120, 71, 4}, {124, 72, 4},
};

void Append(std::vector<BankEvent>& dst, std::vector<BankEvent>&& src)
{
  dst.insert(dst.end(),
             std::make_move_iterator(src.begin()),
             std::make_move_iterator(src.end()));
}

} // namespace

void AutoPlayer::Start(Clock::time_point now)
{
  running_ = true;
  start_ = now;
  last_tick_ = -1;
  active_.clear();
}

void AutoPlayer::Stop()
{
  running_ = false;
  active_.clear();
  last_tick_ = -1;
}

void AutoPlayer::NoteOffKey(VoiceBank& bank, uint8_t key, std::vector<BankEvent>& out)
{
  Append(out, bank.NoteOff(key));
}

void AutoPlayer::FireTick(VoiceBank& bank, int64_t abs_tick, std::vector<BankEvent>& out)
{
  /* Release notes whose duration ended at this tick. */
  for (auto it = active_.begin(); it != active_.end();) {
    if (it->off_tick <= abs_tick) {
      NoteOffKey(bank, it->key, out);
      it = active_.erase(it);
    } else {
      ++it;
    }
  }

  const int64_t loop_tick = abs_tick % kLoopTicks;
  for (const ScoreNote& n : kScore) {
    if (static_cast<int64_t>(n.start) != loop_tick) {
      continue;
    }
    /* Retrig if same key still held from a prior phrase. */
    active_.erase(std::remove_if(active_.begin(),
                                 active_.end(),
                                 [&](const ActiveNote& a) {
                                   if (a.key != n.key) {
                                     return false;
                                   }
                                   NoteOffKey(bank, a.key, out);
                                   return true;
                                 }),
                  active_.end());

    Append(out, bank.NoteOn(n.key));
    ActiveNote an;
    an.key = n.key;
    an.off_tick = abs_tick + static_cast<int64_t>(n.duration);
    active_.push_back(an);
  }
}

std::vector<BankEvent> AutoPlayer::Tick(VoiceBank& bank, Clock::time_point now)
{
  std::vector<BankEvent> out;
  if (!running_) {
    return out;
  }

  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(now - start_).count();
  const int64_t cur_tick = static_cast<int64_t>(elapsed_ms / kTickMs);
  if (cur_tick <= last_tick_) {
    return out;
  }

  /* Catch up missed 16ths so RS485 lag does not desync the phrase. */
  constexpr int64_t kMaxCatchUp = 8;
  int64_t from = last_tick_ + 1;
  if (cur_tick - from > kMaxCatchUp) {
    from = cur_tick - kMaxCatchUp;
  }
  for (int64_t t = from; t <= cur_tick; ++t) {
    FireTick(bank, t, out);
  }
  last_tick_ = cur_tick;
  return out;
}

} // namespace midi_host
