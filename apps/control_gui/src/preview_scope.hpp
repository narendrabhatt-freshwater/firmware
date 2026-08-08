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
  /** Oscilloscope timebase: milliseconds per horizontal division (10 divs). */
  void SetTimeDivMs(float ms_per_div) { time_div_ms_ = ms_per_div; }
  /** Oscilloscope timebase: microseconds per horizontal division (10 divs). */
  void SetTimeDivUs(float us_per_div)
  {
    time_div_ms_ = (us_per_div > 1.f ? us_per_div : 1.f) / 1000.f;
  }
  /** Oscilloscope vertical scale: amplitude units per vertical division. */
  void SetVoltDiv(float units_per_div) { volt_div_ = units_per_div; }

  /** Advance oscillators and fill display buffer (call once per frame). */
  void Render(float sample_rate_hz = 48000.f);

  const std::array<float, kDisplaySamples> &Samples() const { return samples_; }
  float Peak() const { return peak_; }
  float Rms() const { return rms_; }
  float PkPk() const { return pkpk_; }
  int ActiveCount() const { return active_count_; }
  /** Wall-clock span of the display buffer at the last Render() rate. */
  float WindowMs() const { return window_ms_; }
  float TimeDivMs() const { return time_div_ms_; }
  float VoltDiv() const { return volt_div_; }
  /** Scale for waveform plot: ±(volt_div * 4) maps to full height. */
  float DisplayScale() const
  {
    const float s = volt_div_ * 4.f;
    return (s > 0.05f) ? s : 0.05f;
  }

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
  float rms_ = 0.f;
  float pkpk_ = 0.f;
  float window_ms_ = 0.f;
  float time_div_ms_ = 10.f;
  float volt_div_ = 0.25f;
  int active_count_ = 0;
};
