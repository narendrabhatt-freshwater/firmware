/**
 * @file wave_loader.hpp
 * @brief Load wav/raw → 48 kHz, split attack[0,256) + body from sample 224.
 */

#ifndef HOST_IO_AUDIO_WAVE_LOADER_HPP
#define HOST_IO_AUDIO_WAVE_LOADER_HPP

#include "cardlink/audio/sample_dry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cardlink {
namespace audio {

struct LoadedWave {
  std::vector<int32_t> attack; /**< 256 Q31 @ 48 kHz */
  std::vector<int16_t> body;   /**< from kBodyOrigin, 48 kHz int16 */
  uint32_t src_rate_hz = kSampleRateHz;
};

/**
 * @param raw_rate_hz  Used when path is .raw int16 LE (0 → 48 kHz).
 */
bool LoadWaveFile(const std::string &path, uint32_t raw_rate_hz,
                  LoadedWave &out, std::string &err);

/** body.i16 starts at file 256; prepend last K samples of head.i32 as int16. */
bool BodyWithHeadOverlap(const std::string &head_i32_path,
                         const std::string &body_i16_path,
                         std::vector<int16_t> &body_out, std::string &err);

} // namespace audio
} // namespace cardlink

#endif
