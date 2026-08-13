/**
 ******************************************************************************
 * @file    usb_app.h
 * @brief   TinyUSB application layer: UAC2 speaker + CDC console glue.
 *
 * Owns clocks/PHY/NVIC bring-up for the HS device, UAC2 → audio_bridge
 * callbacks, and CDC line assembly that feeds Console_ExecFromUSB().
 ******************************************************************************
 */

#ifndef USB_APP_H
#define USB_APP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Enable USB clocks/NVIC and start the TinyUSB device stack.
     * @note Replaces CubeMX MX_USB_DEVICE_Init(); call once during startup,
     *       inside a USER CODE block so regeneration preserves it.
     */
    void USB_App_Init(void);

    /**
     * @brief Service TinyUSB and the CDC console line buffer.
     * @note Call every main-loop iteration after ChannelConsole_Poll().
     */
    void USB_App_Task(void);

    /**
     * @brief Drain TinyUSB events from the USB ISR so ISO OUT is re-armed
     *        before the next SOF (queued tud_task in the main loop is too late).
     */
    void USB_App_TaskFromIsr(void);

    /**
     * @brief Write a NUL-terminated string to the CDC console.
     * @param s String to send; NULL or closed port is a no-op.
     */
    void USB_CDC_WriteStr(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* USB_APP_H */
