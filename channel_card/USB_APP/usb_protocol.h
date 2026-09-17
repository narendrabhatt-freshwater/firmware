#ifndef CHANNEL_USB_PROTOCOL_H
#define CHANNEL_USB_PROTOCOL_H
#include "usb_stream.h"
#include <string.h>
/* No allocation, delimiter scanning or assumptions about USB transaction sizes. */
typedef struct {
  uint8_t bytes[USB_STREAM_HEADER_BYTES + USB_STREAM_PAYLOAD_MAX];
  uint16_t used, need;
} USB_Parser;
static inline void USB_ParserReset(USB_Parser *p)
{ p->used=0; p->need=USB_STREAM_HEADER_BYTES; }
/* 0 incomplete, 1 complete (caller must reset), -1 invalid header. */
static inline int USB_ParserByte(USB_Parser *p, uint8_t byte)
{
  if (p->used>=p->need || p->need>sizeof p->bytes) return -1;
  p->bytes[p->used++]=byte;
  if (p->used==USB_STREAM_HEADER_BYTES) {
    uint16_t size=USB_Read16(p->bytes+3);
    if (size>USB_STREAM_PAYLOAD_MAX || p->bytes[0]<USB_MSG_HELLO || p->bytes[0]>USB_MSG_PROBE) return -1;
    p->need=(uint16_t)(USB_STREAM_HEADER_BYTES+size);
  }
  return p->used==p->need;
}
#endif
