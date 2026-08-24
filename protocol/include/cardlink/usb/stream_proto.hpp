/**
 * @file stream_proto.hpp
 * @brief Host copy of Channel Card vendor bulk BODY framing.
 *
 * Must match channel_card/USB_APP/usb_stream.h. BODY has no per-packet ACK;
 * vendor IN pushes STATUS (type 0x10), and every refill requires a fresh one.
 * One bulk OUT transfer holds a PACK (type 0x03) of per-voice BODY metas.
 * Vendor ISO is not used (macOS IOUSBHostFamily panics on that path).
 */

#pragma once

#include <cstdint>

namespace cardlink {
namespace usb {

constexpr uint16_t kStreamVid = 0xCafe;
constexpr uint16_t kStreamPid = 0x4022;
constexpr uint8_t kStreamMagic0 = 0x46;
constexpr uint8_t kStreamMagic1 = 0x57;
constexpr uint8_t kStreamTypeBody = 0x01;
constexpr uint8_t kStreamTypePack = 0x03;
constexpr uint8_t kStreamTypeStatus = 0x10;
constexpr unsigned kStreamHdrSize = 8;
constexpr unsigned kStreamBodyMetaSize = 8;
constexpr unsigned kStreamNsampMax = 4096;
constexpr unsigned kStreamSessionMod = 7;
constexpr uint8_t kStreamVendorItf = 0;
constexpr uint8_t kStreamEpOut = 0x01;
constexpr uint8_t kStreamEpIn = 0x81;
constexpr unsigned kStreamFsMps = 64;
/** Conservative measured host/hub service quantum. */
constexpr unsigned kStreamFrameMax = 9472;
constexpr unsigned kStreamServiceFrames = 1;
/** Measured sustainable int16 BODY rates used for capacity classification. */
constexpr unsigned kStreamSustainableSamplesPerMs = 420;
constexpr unsigned kStreamSustainableFewVoiceSamplesPerMs = 437;
/** Measured sweet spot for pipelined macOS/TinyUSB OUT submissions. */
constexpr unsigned kStreamSubmitSamples = 1024;

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

/** Conservative measured service limit, including per-voice card overhead. */
inline constexpr unsigned StreamSustainableSamplesPerMs(unsigned nvoices)
{
  return (nvoices > 0u && nvoices <= 3u)
      ? kStreamSustainableFewVoiceSamplesPerMs
      : kStreamSustainableSamplesPerMs;
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
  uint16_t pad2;
};

struct StreamStatus {
  uint8_t mask;
  uint8_t best;
  uint16_t free_samples[8];
  /** Most recent PACK sequence already reflected by free_samples. */
  uint16_t last_pack_sequence;
};
#pragma pack(pop)

static_assert(sizeof(StreamHdr) == kStreamHdrSize, "hdr");
static_assert(sizeof(StreamBodyMeta) == kStreamBodyMetaSize, "meta");
static_assert(sizeof(StreamStatus) == 20u, "status");

} // namespace usb
} // namespace cardlink
