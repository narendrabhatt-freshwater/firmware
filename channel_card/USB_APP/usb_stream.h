/**
 * @file usb_stream.h
 * @brief Packed BODY protocol carried inside the Channel Card UAC2 stream.
 *
 * The USB interface is class-compliant UAC2 (10ch, int16, 51 kHz).
 * Its bytes are a private request/serve transport, not audible PCM. Every
 * logical PACK is produced from one fresh RS-485 vq permission. PACK CRC32
 * validation makes a missed isochronous packet abort unpublished ring
 * reservations instead of creating a hole in the sample stream.
 */

#ifndef USB_STREAM_H
#define USB_STREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_STREAM_VID 0xCafe
#define USB_STREAM_PID 0x402F

#define USB_STREAM_MAGIC0 0x46u /* 'F' */
#define USB_STREAM_MAGIC1 0x57u /* 'W' */
#define USB_STREAM_TYPE_BODY 0x01u
#define USB_STREAM_TYPE_PACK 0x03u
#define USB_STREAM_HDR_SIZE 8u
#define USB_STREAM_BODY_META_SIZE 8u
#define USB_STREAM_NSAMP_MAX 4096u
#define USB_STREAM_SESSION_MOD 7u

/* USB carrier only: BODY data and DAC playback remain 48 kHz. */
#define USB_STREAM_UAC_CHANNELS 10u
#define USB_STREAM_UAC_SAMPLE_BYTES 2u
#define USB_STREAM_UAC_AUDIO_FRAME_BYTES                              \
  (USB_STREAM_UAC_CHANNELS * USB_STREAM_UAC_SAMPLE_BYTES)
#define USB_STREAM_UAC_RATE_HZ 51000u
#define USB_STREAM_UAC_FRAMES_PER_MS (USB_STREAM_UAC_RATE_HZ / 1000u)
#define USB_STREAM_UAC_PACKET_BYTES                                   \
  (USB_STREAM_UAC_AUDIO_FRAME_BYTES * USB_STREAM_UAC_FRAMES_PER_MS)
/* Synchronous carrier always transmits exactly one nominal packet. */
#define USB_STREAM_UAC_EP_MAX_BYTES USB_STREAM_UAC_PACKET_BYTES
#define USB_STREAM_UAC_WINDOW_PACKETS 10u
#define USB_STREAM_UAC_WINDOW_BYTES                                    \
  (USB_STREAM_UAC_PACKET_BYTES * USB_STREAM_UAC_WINDOW_PACKETS)
#define USB_STREAM_CRC_BYTES 4u
/* Logical PACK bytes before the trailing CRC32. */
#define USB_STREAM_FRAME_MAX                                           \
  (USB_STREAM_UAC_WINDOW_BYTES - USB_STREAM_CRC_BYTES)

#if defined(__GNUC__)
#define USB_STREAM_PACKED __attribute__((packed))
#else
#define USB_STREAM_PACKED
#endif

typedef struct USB_STREAM_PACKED {
  uint8_t magic0;
  uint8_t magic1;
  uint8_t type;
  uint8_t flags;
  uint16_t nbytes;
  uint16_t pad; /* PACK sequence */
} UsbStreamHdr;

typedef struct USB_STREAM_PACKED {
  uint8_t voice;
  uint8_t session;
  uint8_t sof;
  uint8_t pad;
  uint16_t nsamp;
  uint16_t pad2;
} UsbStreamBodyMeta;

#define USB_STREAM_MSG_MAX USB_STREAM_FRAME_MAX

#ifdef __cplusplus
}
#endif

#endif /* USB_STREAM_H */
