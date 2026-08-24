/* TinyUSB configuration — Channel Card
 *
 * Composite device on USB1_OTG_HS (full-speed embedded PHY):
 *   - synchronous UAC2 output, 10ch int16 / 51 kHz BODY carrier
 *   - CDC-ACM console + attack-head load
 *
 * Traffic is exactly 10 x 2 x 51 = 1020 bytes/ms, the largest complete-frame
 * payload below the 1023-byte Full-Speed limit. BODY and DAC remain 48 kHz.
 * class owns endpoint allocation and SET_INTERFACE lifecycle; no custom
 * libusb isochronous pipe is exposed to macOS.
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#include "usb_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU OPT_MCU_STM32H7
#define BOARD_TUD_RHPORT 0
#define CFG_TUSB_OS OPT_OS_NONE

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED
#define CFG_TUSB_MEM_SECTION __attribute__((section(".dma_buffer")))
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_AUDIO 1
#define CFG_TUD_CDC 1
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

#define CFG_TUD_CDC_RX_BUFSIZE 2048
#define CFG_TUD_CDC_TX_BUFSIZE 512

/* UAC2 transport carrier: 10ch int16 at synchronous 51 kHz. */
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN                                      \
  (TUD_AUDIO_SPEAKER_MONO_FB_DESC_LEN -                                   \
   (TUD_AUDIO_DESC_FEATURE_UNIT_ONE_CHANNEL_LEN) -                        \
   TUD_AUDIO_DESC_STD_AS_ISO_FB_EP_LEN)
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT 1
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ 64
#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE USB_STREAM_UAC_RATE_HZ
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX 10
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX 2
#define CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX 16
#define CFG_TUD_AUDIO_ENABLE_EP_OUT 1
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX USB_STREAM_UAC_EP_MAX_BYTES
/* 32 ms absorbs main-loop/RS-485 service jitter without changing wire rate. */
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ                             \
  (32 * CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX)
/* The 31 KB class FIFO is CPU-accessed but does not need DTCM latency. Keep
 * it in USB-accessible AXI SRAM so the interrupt/main stack has real safety
 * margin instead of growing into the CDC parser and UAC state. */
#define CFG_TUD_AUDIO_EP_OUT_SW_BUF_MEM_SECTION CFG_TUSB_MEM_SECTION
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP 0

#if USB_STREAM_UAC_EP_MAX_BYTES > 1023u
#error Full-Speed isochronous OUT endpoint exceeds 1023 bytes
#endif

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
