#include "cardlink/audio/builtin_waves.hpp"

#include "cardlink/audio/sample_dry.hpp"

#include <algorithm>
#include <cmath>

namespace cardlink::audio {

const char *BuiltinWavetableName(uint8_t logical_wave)
{
  static constexpr const char *names[] = {
      "sine", "triangle", "saw", "square",
      "pulse", "organ", "bright", "noise"};
  return logical_wave < kOscillatorWaves ? names[logical_wave] : "unknown";
}

std::vector<int8_t> MakeBuiltinWavetable(uint8_t logical_wave)
{
  constexpr size_t count = 256u;
  constexpr double pi = 3.14159265358979323846;
  std::vector<int8_t> samples(count);
  uint32_t noise = 0x51A7E123u;
  for (size_t i = 0; i < count; ++i) {
    const double phase = static_cast<double>(i) / static_cast<double>(count);
    double value = 0.0;
    switch (logical_wave) {
    case 0: value = std::sin(2.0 * pi * phase); break;
    case 1: value = 1.0 - 4.0 * std::fabs(phase - 0.5); break;
    case 2: value = 2.0 * phase - 1.0; break;
    case 3: value = phase < 0.5 ? 1.0 : -1.0; break;
    case 4: value = phase < 0.25 ? 1.0 : -1.0; break;
    case 5:
      value = (std::sin(2.0 * pi * phase) +
               0.5 * std::sin(4.0 * pi * phase) +
               0.25 * std::sin(6.0 * pi * phase)) /
              1.75;
      break;
    case 6:
      for (int harmonic = 1; harmonic <= 8; ++harmonic) {
        value += std::sin(2.0 * pi * phase * harmonic) /
                 static_cast<double>(harmonic);
      }
      value /= 1.75;
      break;
    default:
      noise = noise * 1664525u + 1013904223u;
      value = static_cast<double>(static_cast<int32_t>(noise >> 16u) - 32768) /
              32768.0;
      break;
    }
    value = std::clamp(value, -1.0, 1.0);
    samples[i] = static_cast<int8_t>(std::lround(value * 127.0));
  }
  return samples;
}

} // namespace cardlink::audio
