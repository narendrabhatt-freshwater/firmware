#ifndef CHANNEL_APP_USB_H
#define CHANNEL_APP_USB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- encode binary usb messages ----------------------------------------- */

/* Channel Card binary CDC protocol, version 2. No USB packet boundaries on wire. */
/* ---- wire constants ----------------------------------------------------- */

#define USB_STREAM_VID 0xCAFEu
#define USB_STREAM_PID 0x4032u
#define USB_STREAM_VERSION 2u
#define USB_STREAM_HEADER_BYTES 5u
#define USB_STREAM_PAYLOAD_MAX 1024u
#define USB_STREAM_SESSION_MOD 255u
#define USB_STREAM_NSAMP_MAX 4096u
#define USB_STREAM_PRIME_SAMPLES 998u

/* Header: type, target, session, payload length LE16.
 * REPLY uses session for the request type. BODY progress is counted implicitly. */
/* ---- message types ------------------------------------------------------ */

enum {
    USB_MSG_HELLO = 1,
    USB_MSG_BODY,
    USB_MSG_UPLOAD_BEGIN,
    USB_MSG_UPLOAD_DATA,
    USB_MSG_UPLOAD_ABORT,
    USB_MSG_REPLY,
    USB_MSG_PROBE
};

enum { USB_UPLOAD_ATTACK = 1, USB_UPLOAD_WAVE, USB_UPLOAD_SCRIPT };

/* ---- read and write little-endian values -------------------------------- */

static inline uint16_t USB_Read16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t USB_Read32(const uint8_t *p)
{
    return (uint32_t)USB_Read16(p) | ((uint32_t)USB_Read16(p + 2) << 16);
}

static inline void USB_Write16(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}

/* ---- parse binary usb messages ------------------------------------------ */

/* No allocation, delimiter scanning or assumptions about USB transaction sizes. */
typedef struct {
    uint8_t bytes[USB_STREAM_HEADER_BYTES + USB_STREAM_PAYLOAD_MAX];
    uint16_t used, need;
} USB_Parser;

static inline void USB_ParserReset(USB_Parser *p)
{
    p->used = 0;
    p->need = USB_STREAM_HEADER_BYTES;
}

/* The caller fills bytes+used, stopping at need, then commits the copied span.
 * 0 incomplete, 1 complete (caller must reset), -1 invalid header/span. */
static inline int USB_ParserCommit(USB_Parser *p, uint16_t count)
{
    if (p->used >= p->need || p->need > sizeof p->bytes || count > p->need - p->used) return -1;
    p->used += count;
    if (p->used == USB_STREAM_HEADER_BYTES) {
        uint16_t size = USB_Read16(p->bytes + 3);
        if (size > USB_STREAM_PAYLOAD_MAX || p->bytes[0] < USB_MSG_HELLO ||
            p->bytes[0] > USB_MSG_PROBE)
            return -1;
        p->need = (uint16_t)(USB_STREAM_HEADER_BYTES + size);
    }
    return p->used == p->need;
}

/* ---- control usb transfers ---------------------------------------------- */

void USB_Device_Init(void);
uint32_t USB_Device_Read(uint8_t *dst, uint32_t size);
uint8_t USB_Device_Write(const uint8_t *src, uint32_t size);
uint32_t USB_Device_TxFree(void);
uint32_t USB_Device_Epoch(void);
uint8_t USB_Device_Connected(void);
void USB_Device_Fault(void);
void USB_Device_Debug(char *text, uint32_t size);
const uint8_t *USB_Descriptor(uint8_t type, uint8_t index, uint16_t *size);

/* ---- upload sample data ------------------------------------------------- */

/* CDC binary load of one signed-int8 attack head. */

uint8_t AttackUpload_IsActive(void);
int AttackUpload_Begin(uint16_t wave_id, uint32_t nbytes);
/** Resolve logical wavetable 0..7 to card-owned attack-bank storage. */
int AttackUpload_BeginWavetable(uint8_t wave, uint32_t nbytes);
uint32_t AttackUpload_Feed(const uint8_t *buf, uint32_t len);
void AttackUpload_Abort(void);

/* ---- process usb traffic ------------------------------------------------ */

/* Binary CDC application layer. Call USB_App_Task from the main loop. */

void USB_App_Init(void);

void USB_App_Task(void);

void USB_CDC_WriteStr(const char *s);

uint32_t USB_App_RxMsgCount(void);
uint32_t USB_App_RxByteCount(void);
uint32_t USB_App_BlockCount(void);
uint32_t USB_App_BadCount(void);
/** Transport faults are counted in reason 4. */
uint32_t USB_App_BadReasonCount(uint8_t reason);
/** Last processed BODY sequence. */
uint16_t USB_App_LastPackSequence(void);
void USB_App_StatsClear(void);

#ifdef __cplusplus
}
#endif

#endif
