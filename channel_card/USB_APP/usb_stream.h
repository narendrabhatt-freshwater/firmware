/* Channel Card binary CDC protocol, version 1. No USB packet boundaries on wire. */
#ifndef USB_STREAM_H
#define USB_STREAM_H
#include <stdint.h>
#define USB_STREAM_VID 0xCAFEu
#define USB_STREAM_PID 0x4032u
#define USB_STREAM_VERSION 1u
#define USB_STREAM_HEADER_BYTES 8u
#define USB_STREAM_PAYLOAD_MAX 1024u
#define USB_STREAM_SESSION_MOD 255u
#define USB_STREAM_NSAMP_MAX 4096u
#define USB_STREAM_PRIME_SAMPLES 998u
#define USB_STREAM_FLAG_START 1u
/* Header: type, target, session, flags, payload length LE16, sequence LE16. */
enum { USB_MSG_HELLO = 1, USB_MSG_BODY, USB_MSG_UPLOAD_BEGIN,
       USB_MSG_UPLOAD_DATA, USB_MSG_UPLOAD_ABORT, USB_MSG_REPLY, USB_MSG_PROBE };
enum { USB_UPLOAD_ATTACK = 1, USB_UPLOAD_WAVE, USB_UPLOAD_SCRIPT };
static inline uint16_t USB_Read16(const uint8_t *p)
{ return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t USB_Read32(const uint8_t *p)
{ return (uint32_t)USB_Read16(p) | ((uint32_t)USB_Read16(p + 2) << 16); }
static inline void USB_Write16(uint8_t *p, uint16_t n)
{ p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); }
#endif
