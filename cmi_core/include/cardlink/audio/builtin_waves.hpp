#pragma once

#include <cstdint>
#include <vector>

namespace cardlink::audio {

/** Stable starter bank used by host tools when no custom oscillator file exists. */
const char *BuiltinWavetableName(uint8_t logical_wave);
std::vector<int8_t> MakeBuiltinWavetable(uint8_t logical_wave);

} // namespace cardlink::audio
