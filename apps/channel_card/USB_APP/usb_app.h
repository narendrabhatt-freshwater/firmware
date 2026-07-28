/* USB application layer — TinyUSB UAC2 speaker (mono 32-bit/96k) + CDC */

#ifndef USB_APP_H
#define USB_APP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Enable USB clocks/NVIC and start the TinyUSB device stack.
 * Replaces MX_USB_DEVICE_Init(); call once during startup. */
void USB_App_Init(void);

/** Service TinyUSB + the CDC console; call every main-loop iteration. */
void USB_App_Task(void);

/** Write a string out the CDC console (no-op if port closed). */
void USB_CDC_WriteStr(const char *s);

/** Implemented in main.c: run one console command line arriving from CDC;
 * replies are routed back to CDC for the duration of the call. */
void Console_ExecFromUSB(char *line);

#ifdef __cplusplus
}
#endif

#endif /* USB_APP_H */
