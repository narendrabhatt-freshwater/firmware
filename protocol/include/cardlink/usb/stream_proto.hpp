/** @file stream_proto.hpp Direct BODY-over-UAC2 audio-frame layout. */
#pragma once

#include <cstddef>
#include <cstdint>

namespace cardlink::usb {

constexpr uint16_t kStreamVid = 0xCafe;
constexpr uint16_t kStreamPid = 0x402F;
/** Full 8-bit session sequence; 0xFF remains the card's unarmed sentinel. */
constexpr unsigned kStreamSessionMod = 255;

constexpr unsigned kStreamUacChannels = 10;
constexpr unsigned kStreamUacSampleBytes = 2;
constexpr unsigned kStreamUacAudioFrameBytes =
    kStreamUacChannels * kStreamUacSampleBytes;
/* USB carrier only. BODY files, pitch demand, and card playback stay 48 kHz. */
constexpr unsigned kStreamUacRateHz = 51000;
constexpr unsigned kStreamUacFramesPerMs = kStreamUacRateHz / 1000u;
constexpr unsigned kStreamUacPacketBytes =
    kStreamUacAudioFrameBytes * kStreamUacFramesPerMs;
constexpr unsigned kStreamUacPacketWords =
    kStreamUacPacketBytes / kStreamUacSampleBytes;
constexpr unsigned kStreamUacBodySamples = kStreamUacPacketWords - 1u;
constexpr uint16_t kStreamTagMask = 0xF000u;
constexpr uint16_t kStreamTagBase = 0xA000u;
constexpr uint16_t kStreamTagIdle = 0xAFFFu;
constexpr uint16_t kStreamTagSof = 0x0008u;
constexpr uint16_t kStreamTagVoiceMask = 0x0007u;
constexpr unsigned kStreamTagSessionShift = 4u;
constexpr uint16_t kStreamTagSessionMask = 0x00FFu;
constexpr unsigned kStreamBodySamplesPerMs = kStreamUacBodySamples;
static_assert(kStreamUacAudioFrameBytes == 20, "10ch signed-int16 frame");
static_assert(kStreamUacRateHz % 1000u == 0u,
              "fixed Full-Speed carrier needs whole frames per millisecond");
static_assert(kStreamUacPacketBytes == 1020u,
              "maximum complete-frame 10ch int16 carrier packet");
static_assert(kStreamUacPacketBytes <= 1023u, "Full-Speed ISO packet limit");
static_assert(kStreamUacPacketWords == 510u, "1020-byte packet as int16 words");
static_assert(kStreamBodySamplesPerMs == 509u,
              "one tag then 509 BODY samples per millisecond");

} // namespace cardlink::usb
