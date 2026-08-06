#include "preview_scope.hpp"

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void PreviewScope::SetVoices(const midi_host::VoiceBank &bank)
{
  const auto &slots = bank.Slots();
  for (uint8_t i = 0; i < midi_host::kVoiceCount; ++i) {
    const bool on = slots[i].active && slots[i].freq_hz > 0.0;
    if (on && !oscs_[i].active) {
      oscs_[i].phase = 0.0;
    }
    oscs_[i].active = on;
    oscs_[i].freq_hz = on ? slots[i].freq_hz : 0.0;
  }
}

void PreviewScope::Render(float sample_rate_hz)
{
  peak_ = 0.f;
  const double two_pi = 2.0 * M_PI;
  for (int n = 0; n < kDisplaySamples; ++n) {
    double mix = 0.0;
    int active = 0;
    for (auto &o : oscs_) {
      if (!o.active) {
        continue;
      }
      ++active;
      mix += std::sin(o.phase);
      o.phase += two_pi * o.freq_hz / static_cast<double>(sample_rate_hz);
      if (o.phase > two_pi) {
        o.phase -= two_pi;
      }
    }
    if (active > 1) {
      mix /= static_cast<double>(active);
    }
    const float s = static_cast<float>(mix);
    samples_[static_cast<std::size_t>(n)] = s;
    peak_ = std::max(peak_, std::fabs(s));
  }
}
