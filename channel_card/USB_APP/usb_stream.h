/**
 * @file usb_stream.h
 * @brief Channel Card vendor bulk BODY framing (host must match).
 *
 * Fire-and-forget: the card never replies to BODY. USB bulk still retries
 * CRC errors and NAKs when the RX FIFO is full — that is the pipe, not an
 * application ACK.
 *
 * Full-Speed bulk MPS is 64. The DCD OUT transfer is many packets
 * (CFG_TUD_VENDOR_EPSIZE) so one main-loop tud_task can take a whole
 * 1 ms frame. Do not put parse or ring writes in the USB ISR.
 */

#ifndef USB_STREAM_H
#define USB_STREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_STREAM_VID 0xCafe
#define USB_STREAM_PID 0x4020

#define USB_STREAM_MAGIC0 0x46u /* 'F' */
#define USB_STREAM_MAGIC1 0x57u /* 'W' */

#define USB_STREAM_TYPE_BODY 0x01u
#define USB_STREAM_TYPE_STATUS 0x10u    /* reserved, not an ACK */
#define USB_STREAM_TYPE_ATTACK 0x02u    /* reserved; attack stays on CDC */
#define USB_STREAM_TYPE_CAPTURE 0x20u   /* reserved (Effect later) */

#define USB_STREAM_HDR_SIZE 8u
#define USB_STREAM_BODY_META_SIZE 8u
#define USB_STREAM_NSAMP_MAX 512u
#define USB_STREAM_SESSION_MOD 7u
#define USB_STREAM_ITF_VENDOR 0u

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
  uint16_t pad;
} UsbStreamHdr;

typedef struct USB_STREAM_PACKED {
  uint8_t voice;
  uint8_t session;
  uint8_t sof;
  uint8_t pad;
  uint16_t nsamp;
  uint16_t pad2;
} UsbStreamBodyMeta;

#define USB_STREAM_MSG_MAX                                                     \
  (USB_STREAM_HDR_SIZE + USB_STREAM_BODY_META_SIZE +                           \
   (USB_STREAM_NSAMP_MAX * 2u))

#ifdef __cplusplus
}
#endif

#endif /* USB_STREAM_H */
