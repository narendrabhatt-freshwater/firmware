/* TinyUSB configuration — Channel Card
 *
 * Composite device on USB1_OTG_HS (full-speed, embedded PHY):
 *   - UAC2 speaker, MONO, 32-bit / 96 kHz -> CS4304 DAC channel 1 via I2S1
 *   - CDC-ACM virtual COM port running the same console as RS485
 *
 * Replaces ST's USB Device Library (USB_DEVICE/, Middlewares/ST) which
 * could only host a single class and therefore no console.
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

/* STM32H725 has a single USB core (USB1_OTG_HS).  TinyUSB's STM32 port
 * aliases it onto controller index 0, so rhport 0 is correct here even
 * though the peripheral and its IRQ are named OTG_HS. */
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
// AUDIO CLASS DRIVER CONFIGURATION (mono 32-bit 96 kHz speaker)
//--------------------------------------------------------------------

#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN         TUD_AUDIO_SPEAKER_MONO_FB_DESC_LEN
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT         1
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ      64

#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE          96000
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX            1
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX    4
#define CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX            32

#define CFG_TUD_AUDIO_ENABLE_EP_OUT                   1
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX                             \
  TUD_AUDIO_EP_SIZE(CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE,              \
                    CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX,        \
                    CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ                          \
  (4 * CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX)

/* Feedback endpoint is present (required by the UAC2 async sink
 * descriptor), but the value is supplied manually as the nominal rate —
 * see tud_audio_feedback_params_cb() in usb_app.c.  The real rate
 * matching is done downstream by the I2S ring buffer's lead/drift guard,
 * which is the mechanism that was already proven on this board. */
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP                  1
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_FORMAT_CORRECTION   1

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
