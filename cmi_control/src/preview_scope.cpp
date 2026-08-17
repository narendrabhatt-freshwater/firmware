#include "preview_scope.hpp"

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void PreviewScope::SetVoices(const cardlink::midi::VoiceBank &bank)
{
  const auto &slots = bank.Slots();
  for (uint8_t i = 0; i < cardlink::midi::kVoiceCount; ++i) {
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
  rms_ = 0.f;
  pkpk_ = 0.f;
  active_count_ = 0;
  for (const auto &o : oscs_) {
    if (o.active) {
      ++active_count_;
    }
  }

  // 10 horizontal divisions × time/div = total window.
  const float window_sec =
      std::max(time_div_ms_, 0.1f) * 10.f / 1000.f;
  window_ms_ = window_sec * 1000.f;
  const int capture_n = std::clamp(
      static_cast<int>(window_sec * sample_rate_hz), 32, 65536);

  // Decimate capture into display buffer.
  float min_s = 0.f;
  float max_s = 0.f;
  double sum_sq = 0.0;
  const double two_pi = 2.0 * M_PI;

  // Advance oscillators for one window, sampling into display points.
  for (int di = 0; di < kDisplaySamples; ++di) {
    const int start = (di * capture_n) / kDisplaySamples;
    const int end = ((di + 1) * capture_n) / kDisplaySamples;
    float bucket_min = 0.f;
    float bucket_max = 0.f;
    bool first = true;
    double bucket_sum = 0.0;
    int bucket_count = 0;
    for (int n = start; n < end; ++n) {
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
      if (first) {
        bucket_min = bucket_max = s;
        first = false;
      } else {
        bucket_min = std::min(bucket_min, s);
        bucket_max = std::max(bucket_max, s);
      }
      bucket_sum += s;
      ++bucket_count;
      peak_ = std::max(peak_, std::fabs(s));
      sum_sq += static_cast<double>(s) * static_cast<double>(s);
      if (n == 0 && di == 0) {
        min_s = max_s = s;
      }
      min_s = std::min(min_s, s);
      max_s = std::max(max_s, s);
    }
    // Prefer peak in bucket so fast edges remain visible when zoomed out.
    const float mid =
        bucket_count > 0 ? static_cast<float>(bucket_sum / bucket_count) : 0.f;
    samples_[static_cast<std::size_t>(di)] =
        (std::fabs(bucket_max) >= std::fabs(bucket_min)) ? bucket_max
                                                         : (first ? mid : bucket_min);
  }

  pkpk_ = max_s - min_s;
  const int total = std::max(capture_n, 1);
  rms_ = static_cast<float>(std::sqrt(sum_sq / static_cast<double>(total)));
}
