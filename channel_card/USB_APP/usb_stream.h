/**
 * @file usb_stream.h
 * @brief Direct BODY samples carried in each 1 ms Channel Card UAC2 packet.
 *
 * The USB interface is class-compliant UAC2 (21ch, int8, 48 kHz).
 * Each 1 ms USB packet has four metadata bytes followed by 1004 signed BODY
 * samples.
 * USB supplies the packet boundary; there is no inner header, length, or CRC.
 */

#ifndef USB_STREAM_H
#define USB_STREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_STREAM_VID 0xCafe
#define USB_STREAM_PID 0x4031

/* Full 8-bit sequence; 0xFF is reserved as the unarmed sentinel. */
#define USB_STREAM_SESSION_MOD 255u
#define USB_STREAM_NSAMP_MAX 4096u

#define USB_STREAM_UAC_CHANNELS 21u
#define USB_STREAM_UAC_SAMPLE_BYTES 1u
#define USB_STREAM_UAC_AUDIO_FRAME_BYTES                              \
  (USB_STREAM_UAC_CHANNELS * USB_STREAM_UAC_SAMPLE_BYTES)
#define USB_STREAM_UAC_RATE_HZ 48000u
#define USB_STREAM_UAC_FRAMES_PER_MS (USB_STREAM_UAC_RATE_HZ / 1000u)
#define USB_STREAM_UAC_PACKET_BYTES                                   \
  (USB_STREAM_UAC_AUDIO_FRAME_BYTES * USB_STREAM_UAC_FRAMES_PER_MS)
#define USB_STREAM_UAC_EP_MAX_BYTES USB_STREAM_UAC_PACKET_BYTES

/* byte 0 = 0xA0 | SOF[3] | voice[2:0]; byte 1 = session. */
#define USB_STREAM_TAG_MASK 0xF0u
#define USB_STREAM_TAG_BASE 0xA0u
#define USB_STREAM_TAG_IDLE 0xFFu
#define USB_STREAM_TAG_SOF 0x08u
#define USB_STREAM_TAG_VOICE_MASK 0x07u
#define USB_STREAM_UAC_HEADER_BYTES 4u
#define USB_STREAM_UAC_BODY_SAMPLES \
  (USB_STREAM_UAC_PACKET_BYTES - USB_STREAM_UAC_HEADER_BYTES)

#ifdef __cplusplus
}
#endif

#endif /* USB_STREAM_H */
