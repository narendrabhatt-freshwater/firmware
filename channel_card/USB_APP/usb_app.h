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
     * @brief Drain UAC into the body rings and poll the CDC line buffer.
     * @note TinyUSB itself is serviced from the USB ISR. Call again after
     *       ChannelConsole_Poll() so RS485 TX cannot sit on a full ISO FIFO.
     */
    void USB_App_Task(void);

    /**
     * @brief Re-arm ISO OUT from the USB ISR (tud_task only; no ring demux).
     */
    void USB_App_TaskFromIsr(void);

    /**
     * @brief Write a NUL-terminated string to the CDC console.
     * @param s String to send; NULL or closed port is a no-op.
     */
    void USB_CDC_WriteStr(const char *s);

    uint32_t USB_App_IsoDropCount(void);
    uint32_t USB_App_IsoIncompCount(void);
    void USB_App_PktSizeCounts(uint32_t *n47, uint32_t *n48, uint32_t *n49,
                               uint32_t *nxx);
    void USB_App_StatsClear(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_APP_H */
