/* TinyUSB configuration — Channel Card
 *
 * Composite device on USB1_OTG_HS (full-speed, embedded PHY):
 *   - UAC2 speaker, 10ch int16 / 48 kHz tagged body → per-voice slot rings
 *   - CDC-ACM console + attack-head binary load
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUSB_MCU                OPT_MCU_STM32H7

#define BOARD_TUD_RHPORT            0

#define CFG_TUSB_OS                 OPT_OS_NONE

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG              0
#endif

#define CFG_TUD_ENABLED             1
/* Embedded PHY on this part is full-speed only */
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUD_ENDPOINT0_SIZE      64

#define CFG_TUD_AUDIO               1
#define CFG_TUD_CDC                 1
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

//--------------------------------------------------------------------
// CDC CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUD_CDC_RX_BUFSIZE      2048
#define CFG_TUD_CDC_TX_BUFSIZE      512

//--------------------------------------------------------------------
// AUDIO: 10ch int16 @ 48 kHz (~960 B/ms; FS ISO max 1023)
//--------------------------------------------------------------------

/* Must match TUD_AUDIO_SPEAKER_10CH_FB_DESC_LEN in usb_descriptors.c.
 * audiod_open() returns this to usbd set_config; wrong length breaks CDC.
 * Parenthesize ONE_CHANNEL_LEN — it is `6+(1+1)*4` without its own parens. */
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN                                        \
  (TUD_AUDIO_SPEAKER_MONO_FB_DESC_LEN -                                      \
   (TUD_AUDIO_DESC_FEATURE_UNIT_ONE_CHANNEL_LEN) + (6 + (10 + 1) * 4))
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT         1
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ      64

#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE          48000
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX            10
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX    2
#define CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX            16

#define CFG_TUD_AUDIO_ENABLE_EP_OUT                   1
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX                             \
  TUD_AUDIO_EP_SIZE(CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE,              \
                    CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX,        \
                    CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)
/* ISO has no retry. 4 packets (~4 ms) was smaller than a CoreAudio 1024-frame
 * period (~21 ms) and smaller than an RS485 vq reply stall. 32 ms of FIFO
 * holds a full host callback until tud_task drains it into stream_ring. */
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ                          \
  (32 * CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX)

#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP                  1
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_FORMAT_CORRECTION   1

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
