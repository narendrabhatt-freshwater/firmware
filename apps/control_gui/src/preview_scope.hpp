#pragma once

#include "voice_bank.hpp"

#include <array>
#include <cstdint>
#include <vector>

/** Host-side mixed sine preview for the scope widget (not card DSP). */
class PreviewScope
{
public:
  static constexpr int kDisplaySamples = 512;

  void SetVoices(const midi_host::VoiceBank &bank);
  /** Advance oscillators and fill display buffer (call once per frame). */
  void Render(float sample_rate_hz = 48000.f);

  const std::array<float, kDisplaySamples> &Samples() const { return samples_; }
  float Peak() const { return peak_; }

private:
  struct Osc
  {
    bool active = false;
    double phase = 0.0;
    double freq_hz = 0.0;
  };

  std::array<Osc, midi_host::kVoiceCount> oscs_{};
  std::array<float, kDisplaySamples> samples_{};
  float peak_ = 0.f;
};
