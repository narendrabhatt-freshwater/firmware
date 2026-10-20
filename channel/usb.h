/*
                                   __
                               ___  \  \
                          ___  \  \  \  \    _______
                     ___  \  \  \  \  \__\__/_____  \
                     \  \  \  \  \__\_______      \__\____
                      \  \  \  \     ___ \  \________  \__/
                       \  \  \__\___/_  \ \___/   \  \___
                        \  \    ____  \  \________ \ ___/
                         \__\__/ \  \  \____/  \  \
                                  \  \     ___  \  \___
                                   \__\___/ \  \ \____/
                                           \  \
                                              \  \___
                 __                 _          \ ___/  _
                / _|_ __  ___  ___ | |____      ____ _| |_  ___  _ __
               | |_| '_ |/ _ \/ __|| '_ \ \ /\ / / _` | __|/ _ \| '__|
               |  _| |  |  __/\__ \| | | \ V  V / (_| | |_|  __/| |
               |_| |_|   \___||___/|_| |_|\_/\_/ \__,_|\__|\___||_|

            (C) 2 0 2 6   F r e s h w a t e r   I n s t r u m e n t s
*/

#ifndef CHANNEL_APP_USB_H
#define CHANNEL_APP_USB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- encode binary usb messages ----------------------------------------- */

/* Channel Card binary CDC protocol. No USB packet boundaries on wire. */
/* ---- wire constants ----------------------------------------------------- */

#define USB_STREAM_HEADER_BYTES 5u
#define USB_UPLOAD_HEADER_BYTES 7u
#define USB_UPLOAD_BYTES_MAX 16404u
#define USB_REPLY_HEADER_BYTES 3u
#define USB_STREAM_PAYLOAD_MAX 1024u
#define USB_STREAM_SESSION_MOD 255u
#define USB_STREAM_NSAMP_MAX 4096u
#define USB_STREAM_PRIME_SAMPLES 998u

/* ---- message types ------------------------------------------------------ */

enum
{
    USB_MSG_BODY = 1,
    USB_MSG_UPLOAD = 2
};

enum
{
    USB_UPLOAD_ATTACK = 1,
    USB_UPLOAD_WAVE,
    USB_UPLOAD_SCRIPT
};

/* ---- read and write little-endian values -------------------------------- */

static inline uint16_t read_usb_uint16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read_usb_uint32_le(const uint8_t *p)
{
    return (uint32_t)read_usb_uint16_le(p) | ((uint32_t)read_usb_uint16_le(p + 2) << 16);
}

static inline void write_usb_uint16_le(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}

/* ---- parse binary usb messages ------------------------------------------ */

/* No allocation, delimiter scanning or assumptions about USB transaction sizes. */
typedef struct
{
    uint8_t bytes[USB_STREAM_HEADER_BYTES + USB_STREAM_PAYLOAD_MAX];
    uint16_t used;
    uint16_t need;
} USB_Parser;

static inline void reset_usb_parser(USB_Parser *p)
{
    p->used = 0;
    p->need = 1;
}

/* ---- collect a BODY frame or the seven-byte UPLOAD header ---------------- */

static inline int commit_usb_parser_bytes(USB_Parser *p, uint16_t count)
{
    if (p->used >= p->need || p->need > sizeof p->bytes || count > p->need - p->used)
    {
        return -1;
    }
    p->used += count;
    if (p->used == 1 && p->need == 1)
    {
        if (p->bytes[0] == USB_MSG_BODY)
        {
            p->need = USB_STREAM_HEADER_BYTES;
        }
        else if (p->bytes[0] == USB_MSG_UPLOAD)
        {
            p->need = USB_UPLOAD_HEADER_BYTES;
        }
        else
        {
            return -1;
        }
    }
    if (p->bytes[0] == USB_MSG_BODY && p->used == USB_STREAM_HEADER_BYTES &&
        p->need == USB_STREAM_HEADER_BYTES)
    {
        uint16_t size = read_usb_uint16_le(p->bytes + 3);
        if (size > USB_STREAM_PAYLOAD_MAX)
        {
            return -1;
        }
        p->need = (uint16_t)(USB_STREAM_HEADER_BYTES + size);
    }
    return p->used == p->need;
}

/* ---- upload sample data ------------------------------------------------- */

/* CDC binary load of one signed-int8 attack head. */

uint8_t is_sample_upload_active(void);
int begin_sample_upload(uint16_t wave_id, uint32_t nbytes);
/** Resolve logical wavetable 0..7 to card-owned attack-bank storage. */
int begin_wavetable_upload(uint8_t wave, uint32_t nbytes);
uint32_t append_sample_upload_bytes(const uint8_t *buf, uint32_t len);
void abort_sample_upload(void);

/* ---- process usb traffic ------------------------------------------------ */

/* Binary CDC application layer. Call process_usb from the main loop. */

void process_usb_messages(void);

void write_usb_text(const char *s);

uint32_t get_usb_received_body_count(void);
uint32_t get_usb_received_byte_count(void);
uint32_t get_usb_received_frame_count(void);
uint32_t get_usb_error_count(void);
/** Transport faults are counted in reason 4. */
uint32_t get_usb_error_reason_count(uint8_t reason);
/** Last processed BODY sequence. */
uint16_t get_usb_last_body_sequence(void);
void clear_usb_statistics(void);

#ifdef __cplusplus
}
#endif

#endif
