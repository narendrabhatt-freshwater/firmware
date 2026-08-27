/** @file stream_proto.hpp Host copy of packed BODY-over-UAC2 framing. */
#pragma once

#include <cstddef>
#include <cstdint>

namespace cardlink::usb {

constexpr uint16_t kStreamVid = 0xCafe;
constexpr uint16_t kStreamPid = 0x402F;
constexpr uint8_t kStreamMagic0 = 0x46;
constexpr uint8_t kStreamMagic1 = 0x57;
constexpr uint8_t kStreamTypeBody = 0x01;
constexpr uint8_t kStreamTypePack = 0x03;
constexpr unsigned kStreamHdrSize = 8;
constexpr unsigned kStreamBodyMetaSize = 8;
constexpr unsigned kStreamNsampMax = 4096;
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
constexpr unsigned kStreamUacWindowPackets = 10;
constexpr unsigned kStreamUacWindowBytes =
    kStreamUacPacketBytes * kStreamUacWindowPackets;
constexpr unsigned kStreamCrcBytes = 4;
constexpr unsigned kStreamFrameMax =
    kStreamUacWindowBytes - kStreamCrcBytes;

inline constexpr unsigned PackMaxSamples(unsigned nvoices)
{
  if (nvoices == 0 || nvoices > 8) {
    return 0;
  }
  const unsigned overhead = kStreamHdrSize + nvoices * kStreamBodyMetaSize;
  if (overhead >= kStreamFrameMax) {
    return 0;
  }
  const unsigned wire = (kStreamFrameMax - overhead) / 2u;
  const unsigned body = nvoices * kStreamNsampMax;
  return (wire < body) ? wire : body;
}

/** 0xFFFF is the vq "no PACK applied" sentinel and is never transmitted. */
inline constexpr uint16_t NextPackSequence(uint16_t sequence)
{
  return sequence == 0xFFFEu ? 0u : static_cast<uint16_t>(sequence + 1u);
}

inline uint32_t StreamCrc32(const uint8_t *data, std::size_t size)
{
  uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8u; ++bit) {
      crc = (crc >> 1u) ^
            (0xEDB88320u & static_cast<uint32_t>(
                                 -static_cast<int32_t>(crc & 1u)));
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

#pragma pack(push, 1)
struct StreamHdr {
  uint8_t magic0;
  uint8_t magic1;
  uint8_t type;
  uint8_t flags;
  uint16_t nbytes;
  uint16_t pad;
};

struct StreamBodyMeta {
  uint8_t voice;
  uint8_t session;
  uint8_t sof;
  uint8_t pad;
  uint16_t nsamp;
  uint16_t wave_id;
};

#pragma pack(pop)

static_assert(sizeof(StreamHdr) == kStreamHdrSize, "hdr");
static_assert(sizeof(StreamBodyMeta) == kStreamBodyMetaSize, "meta");
static_assert(kStreamUacAudioFrameBytes == 20, "10ch signed-int16 frame");
static_assert(kStreamUacRateHz % 1000u == 0u,
              "fixed Full-Speed carrier needs whole frames per millisecond");
static_assert(kStreamUacPacketBytes == 1020u,
              "maximum complete-frame 10ch int16 carrier packet");
static_assert(kStreamUacPacketBytes <= 1023u, "Full-Speed ISO packet limit");
static_assert(kStreamFrameMax + kStreamCrcBytes == kStreamUacWindowBytes,
              "10 ms UAC service window");

} // namespace cardlink::usb
