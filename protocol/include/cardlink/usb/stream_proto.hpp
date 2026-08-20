/**
 * @file stream_proto.hpp
 * @brief Host copy of Channel Card vendor bulk BODY framing.
 *
 * Must match channel_card/USB_APP/usb_stream.h. Fire-and-forget: no ACK.
 */

#pragma once

#include <cstdint>

namespace cardlink {
namespace usb {

constexpr uint16_t kStreamVid = 0xCafe;
constexpr uint16_t kStreamPid = 0x4020;
constexpr uint8_t kStreamMagic0 = 0x46;
constexpr uint8_t kStreamMagic1 = 0x57;
constexpr uint8_t kStreamTypeBody = 0x01;
constexpr unsigned kStreamHdrSize = 8;
constexpr unsigned kStreamBodyMetaSize = 8;
constexpr unsigned kStreamNsampMax = 512;
constexpr unsigned kStreamSessionMod = 7;
constexpr uint8_t kStreamVendorItf = 0;
constexpr uint8_t kStreamEpOut = 0x01;
constexpr unsigned kStreamFsMps = 64;

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
#pragma pack(pop)

static_assert(sizeof(StreamHdr) == kStreamHdrSize, "hdr");
static_assert(sizeof(StreamBodyMeta) == kStreamBodyMetaSize, "meta");

} // namespace usb
} // namespace cardlink
