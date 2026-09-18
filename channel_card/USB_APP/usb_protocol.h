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
/* The caller fills bytes+used, stopping at need, then commits the copied span.
 * 0 incomplete, 1 complete (caller must reset), -1 invalid header/span. */
static inline int USB_ParserCommit(USB_Parser *p, uint16_t count)
{
  if (p->used>=p->need || p->need>sizeof p->bytes || count>p->need-p->used) return -1;
  p->used+=count;
  if (p->used==USB_STREAM_HEADER_BYTES) {
    uint16_t size=USB_Read16(p->bytes+3);
    if (size>USB_STREAM_PAYLOAD_MAX || p->bytes[0]<USB_MSG_HELLO || p->bytes[0]>USB_MSG_PROBE) return -1;
    p->need=(uint16_t)(USB_STREAM_HEADER_BYTES+size);
  }
  return p->used==p->need;
}
#endif
