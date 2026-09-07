/**
 * @file usb_stream.h
 * @brief Direct BODY samples carried in each 1 ms Channel Card UAC2 packet.
 *
 * The USB interface is class-compliant UAC2 (21ch, int8, 48 kHz).
 * Each 1 ms USB packet has ten metadata bytes followed by up to 998 signed BODY
 * samples.
 * The fixed header carries two counted blocks, without a CRC.
 */

#ifndef USB_STREAM_H
#define USB_STREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_STREAM_VID 0xCafe
#define USB_STREAM_PID 0x4031

/* Session identities 0..254; 0xFF is the unarmed sentinel. */
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

/* Payload: sequence u16, then two descriptors at offsets 2 and 6.
 * Descriptor: tag = 0xA0 | SOF[3] | voice[2:0], session u8, count u16. */
#define USB_STREAM_TAG_MASK 0xF0u
#define USB_STREAM_TAG_BASE 0xA0u
#define USB_STREAM_TAG_IDLE 0xFFu
#define USB_STREAM_TAG_SOF 0x08u
#define USB_STREAM_TAG_VOICE_MASK 0x07u
#define USB_STREAM_UAC_HEADER_BYTES 10u
#define USB_STREAM_UAC_BODY_SAMPLES \
  (USB_STREAM_UAC_PACKET_BYTES - USB_STREAM_UAC_HEADER_BYTES)

#ifdef __cplusplus
}
#endif

#endif /* USB_STREAM_H */
