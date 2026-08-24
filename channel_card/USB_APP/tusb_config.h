/* TinyUSB configuration — Channel Card
 *
 * Composite device on USB1_OTG_HS (full-speed, embedded PHY):
 *   - Vendor bulk BODY stream (host → per-voice rings), fire-and-forget
 *   - CDC-ACM console + attack-head binary load
 *
 * FS bulk MPS is 64. CFG_TUD_VENDOR_EPSIZE is the DCD OUT buffer so one
 * RS-485-vq-permitted refill can span 148 packets. This deliberately
 * amortizes the measured host/hub round-trip. The vendor IN endpoint remains
 * descriptor-compatible but idle; BODY traffic is vendor OUT only.
 *
 * Do not put an isochronous endpoint on the vendor interface: macOS
 * IOUSBHostFamily panics on claim_interface / ISO submit for that class.
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
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUD_ENDPOINT0_SIZE      64

#define CFG_TUD_AUDIO               0
#define CFG_TUD_CDC                 1
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              1

//--------------------------------------------------------------------
// CDC CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUD_CDC_RX_BUFSIZE      2048
#define CFG_TUD_CDC_TX_BUFSIZE      512

//--------------------------------------------------------------------
// VENDOR BULK: max FS frame into one OUT xfer
//--------------------------------------------------------------------

#define CFG_TUD_VENDOR_EPSIZE       9472
#define CFG_TUD_VENDOR_RX_BUFSIZE   32768
#define CFG_TUD_VENDOR_TX_BUFSIZE   64

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
