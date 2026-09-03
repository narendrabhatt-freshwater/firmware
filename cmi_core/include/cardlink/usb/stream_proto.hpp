/** @file stream_proto.hpp Direct BODY-over-UAC2 audio-frame layout. */
#pragma once

#include <cstddef>
#include <cstdint>

namespace cardlink::usb {

constexpr uint16_t kStreamVid = 0xCafe;
constexpr uint16_t kStreamPid = 0x4031;
/** Full 8-bit session sequence; 0xFF remains the card's unarmed sentinel. */
constexpr unsigned kStreamSessionMod = 255;

constexpr unsigned kStreamUacChannels = 21;
constexpr unsigned kStreamUacSampleBytes = 1;
constexpr unsigned kStreamUacAudioFrameBytes =
    kStreamUacChannels * kStreamUacSampleBytes;
/* 21 is the most whole signed-int8 channels that fit at 48 frames/ms. */
constexpr unsigned kStreamUacRateHz = 48000;
constexpr unsigned kStreamUacFramesPerMs = kStreamUacRateHz / 1000u;
constexpr unsigned kStreamUacPacketBytes =
    kStreamUacAudioFrameBytes * kStreamUacFramesPerMs;
constexpr unsigned kStreamUacHeaderBytes = 4u;
constexpr unsigned kStreamUacBodySamples =
    kStreamUacPacketBytes - kStreamUacHeaderBytes;
constexpr uint8_t kStreamTagMask = 0xF0u;
constexpr uint8_t kStreamTagBase = 0xA0u;
constexpr uint8_t kStreamTagIdle = 0xFFu;
constexpr uint8_t kStreamTagSof = 0x08u;
constexpr uint8_t kStreamTagVoiceMask = 0x07u;
constexpr unsigned kStreamBodySamplesPerMs = kStreamUacBodySamples;
static_assert(kStreamUacAudioFrameBytes == 21, "21ch signed-int8 frame");
static_assert(kStreamUacRateHz % 1000u == 0u,
              "fixed Full-Speed carrier needs whole frames per millisecond");
static_assert(kStreamUacPacketBytes == 1008u,
              "maximum 48 kHz whole-frame int8 carrier packet");
static_assert(kStreamUacPacketBytes <= 1023u, "Full-Speed ISO packet limit");
static_assert(kStreamBodySamplesPerMs == 1004u,
              "four metadata bytes then 1004 BODY samples per millisecond");

} // namespace cardlink::usb
