/**
 ******************************************************************************
 * @file    usb_app.h
 * @brief   TinyUSB application layer: vendor bulk BODY + CDC console.
 *
 * USB ISR is DCD only (tud_int_handler). tud_task and BODY parse run in
 * USB_App_Task() from the main loop. A full vendor RX FIFO NAKs the host.
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
     * @brief Run TinyUSB, drain BODY into the rings, poll CDC.
     * @note Call from main. Call again after ChannelConsole_Poll() so a
     *       long RS485 TX cannot sit on a full vendor RX FIFO.
     */
    void USB_App_Task(void);

    /**
     * @brief Write a NUL-terminated string to the CDC console.
     * @param s String to send; NULL or closed port is a no-op.
     */
    void USB_CDC_WriteStr(const char *s);

    uint32_t USB_App_RxMsgCount(void);
    uint32_t USB_App_RxByteCount(void);
    uint32_t USB_App_BadCount(void);
    void USB_App_StatsClear(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_APP_H */
