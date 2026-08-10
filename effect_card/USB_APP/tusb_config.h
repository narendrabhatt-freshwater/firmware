/* TinyUSB configuration — Effect Card
 *
 * Composite device on OTG_FS (Full-Speed, embedded PHY):
 *   - UAC2 microphone, MONO, 32-bit / 96 kHz  (384 B/ms of the 1023 B/ms
 *     FS isochronous budget).  Which of the 8 ADC channels feeds the
 *     stream is selected at runtime (`u <1..8>` console command).
 *   - CDC-ACM virtual COM port running the same debug console as RS485.
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
#define BOARD_TUD_RHPORT            0 /* OTG_FS */
#define CFG_TUSB_OS                 OPT_OS_NONE

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG              0
#endif

#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUD_ENDPOINT0_SIZE      64

//------------- CLASS -------------//
#define CFG_TUD_AUDIO               1
#define CFG_TUD_CDC                 1
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

//--------------------------------------------------------------------
// CDC CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUD_CDC_RX_BUFSIZE      256
#define CFG_TUD_CDC_TX_BUFSIZE      512

//--------------------------------------------------------------------
// AUDIO CLASS DRIVER CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE            96000

#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN               TUD_AUDIO_MIC_ONE_CH_DESC_LEN
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT               1
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ            64

#define CFG_TUD_AUDIO_ENABLE_EP_IN                  1
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX  4 /* 32-bit slots  */
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX          1 /* mono          */

/* EP size MUST stay at the computed value (97 samples / 388 B), not a
 * bigger round number.  TinyUSB sends min(FIFO content, EP size) per IN
 * token, so the EP size is what rate-limits the drain: 97 lets the
 * stream recover 1 sample/frame after a hiccup while never running the
 * FIFO dry.  Enlarging it to 128 samples made the drain outpace the
 * producer (128/64/128/64 instead of a steady 96) — visible as ragged
 * dropouts even in a constant-amplitude test tone. */
#define CFG_TUD_AUDIO_EP_SZ_IN                                         \
  TUD_AUDIO_EP_SIZE(CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE,                  \
                    CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX,        \
                    CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX)

#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX           CFG_TUD_AUDIO_EP_SZ_IN
/* 16 ms of buffering: absorbs SAI-vs-USB clock drift and IRQ jitter */
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ        (16 * CFG_TUD_AUDIO_EP_SZ_IN)

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
