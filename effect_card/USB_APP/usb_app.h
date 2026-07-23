/* USB application layer — TinyUSB UAC2 mic (mono 32-bit/96k) + CDC console */

#ifndef USB_APP_H
#define USB_APP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Init TinyUSB device stack (call once after MX_USB_OTG_FS_PCD_Init). */
void USB_App_Init(void);

/** Service TinyUSB + the CDC console; call every main-loop iteration. */
void USB_App_Task(void);

/** Write a string out the CDC console (no-op if port closed). */
void USB_CDC_WriteStr(const char *s);

/** Push captured audio samples into the USB mic stream FIFO.
 * Safe to call from ISR (SAI DMA callbacks). */
void USB_Audio_Write(const uint8_t *data, uint16_t len);


/** Which ADC channel (1..8) feeds the USB mono stream.
 * 1..4 = ADC1 (0x4C) ch1..4, 5..8 = ADC2 (0x4D) ch1..4. */
extern volatile uint8_t usb_adc_ch;

/** Implemented in main.c: run one console command line arriving from CDC;
 * replies are routed back to CDC for the duration of the call. */
void Console_ExecFromUSB(char *line);

#ifdef __cplusplus
}
#endif

#endif /* USB_APP_H */
