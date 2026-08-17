/**
 * @file wave_loader.hpp
 * @brief Load wav/raw → 48 kHz, split attack[0, min(n,kAttackSamples)) + body.
 */

#ifndef CARDLINK_AUDIO_WAVE_LOADER_HPP
#define CARDLINK_AUDIO_WAVE_LOADER_HPP

#include "cardlink/audio/sample_dry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cardlink {
namespace audio {

struct LoadedWave {
  std::vector<int32_t> attack; /**< Q31 @ 48 kHz, length ≤ kAttackSamples */
  std::vector<int16_t> body;   /**< from (len-overlap), 48 kHz int16 */
  uint32_t src_rate_hz = kSampleRateHz;
};

/**
 * @param raw_rate_hz  Used when path is .raw int16 LE (0 → 48 kHz).
 */
bool LoadWaveFile(const std::string &path, uint32_t raw_rate_hz,
                  LoadedWave &out, std::string &err);

/** Prepend last overlap of the real head (Q31>>16), no hold-pad. */
bool BodyWithHeadOverlap(const std::string &head_i32_path,
                         const std::string &body_i16_path,
                         std::vector<int16_t> &body_out, std::string &err);

} // namespace audio
} // namespace cardlink

#endif /* CARDLINK_AUDIO_WAVE_LOADER_HPP */
