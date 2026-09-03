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
  std::vector<int8_t> attack; /**< signed PCM @ 48 kHz, length ≤ kAttackSamples */
  std::vector<int8_t> body;   /**< from (len-overlap), 48 kHz signed int8 */
  uint32_t src_rate_hz = kSampleRateHz;
};

/**
 * @param raw_rate_hz  Used when path is signed .raw int8 (0 → 48 kHz).
 */
bool LoadWaveFile(const std::string &path, uint32_t raw_rate_hz,
                  LoadedWave &out, std::string &err);

/** Prepend the last overlap of a signed-int8 head, with no hold-pad. */
bool BodyWithHeadOverlap(const std::string &head_i8_path,
                         const std::string &body_i8_path,
                         std::vector<int8_t> &body_out, std::string &err);

} // namespace audio
} // namespace cardlink

#endif /* CARDLINK_AUDIO_WAVE_LOADER_HPP */
