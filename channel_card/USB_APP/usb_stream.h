/**
 * @file usb_stream.h
 * @brief Direct BODY samples carried in each Channel Card UAC2 audio frame.
 *
 * The USB interface is class-compliant UAC2 (10ch, int16, 51 kHz).
 * Each 10-channel int16 audio frame is one routing tag followed by nine raw
 * BODY samples. USB already supplies the frame boundary; there is no inner
 * header, length, PACK window, or CRC.
 */

#ifndef USB_STREAM_H
#define USB_STREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_STREAM_VID 0xCafe
#define USB_STREAM_PID 0x402F

/* Full 8-bit sequence; 0xFF is reserved as the unarmed sentinel. */
#define USB_STREAM_SESSION_MOD 255u
#define USB_STREAM_NSAMP_MAX 4096u

/* USB carrier only: BODY data and DAC playback remain 48 kHz. */
#define USB_STREAM_UAC_CHANNELS 10u
#define USB_STREAM_UAC_SAMPLE_BYTES 2u
#define USB_STREAM_UAC_AUDIO_FRAME_BYTES                              \
  (USB_STREAM_UAC_CHANNELS * USB_STREAM_UAC_SAMPLE_BYTES)
#define USB_STREAM_UAC_RATE_HZ 51000u
#define USB_STREAM_UAC_FRAMES_PER_MS (USB_STREAM_UAC_RATE_HZ / 1000u)
#define USB_STREAM_UAC_PACKET_BYTES                                   \
  (USB_STREAM_UAC_AUDIO_FRAME_BYTES * USB_STREAM_UAC_FRAMES_PER_MS)
#define USB_STREAM_UAC_EP_MAX_BYTES USB_STREAM_UAC_PACKET_BYTES

/* tag = 0xA000 | session[11:4] | SOF[3] | voice[2:0]. */
#define USB_STREAM_TAG_MASK 0xF000u
#define USB_STREAM_TAG_BASE 0xA000u
#define USB_STREAM_TAG_IDLE 0xAFFFu
#define USB_STREAM_TAG_SOF 0x0008u
#define USB_STREAM_TAG_VOICE_MASK 0x0007u
#define USB_STREAM_TAG_SESSION_SHIFT 4u
#define USB_STREAM_TAG_SESSION_MASK 0x00FFu
#define USB_STREAM_UAC_BODY_SAMPLES (USB_STREAM_UAC_CHANNELS - 1u)

#ifdef __cplusplus
}
#endif

#endif /* USB_STREAM_H */
