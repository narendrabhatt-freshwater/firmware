/**
 ******************************************************************************
 * @file    usb_app.h
 * @brief   TinyUSB application layer: UAC2 BODY + CDC console.
 *
 * TinyUSB task service runs from the USB ISR so UAC ISO OUT is re-armed before
 * the next SOF. BODY parsing and CDC line handling run in USB_App_Task().
 ******************************************************************************
 */

#ifndef USB_APP_H
#define USB_APP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    void USB_App_Init(void);

    /**
     * @brief Drain UAC BODY into the rings and poll CDC.
     * @note Call from main. Call again after ChannelConsole_Poll() so a
     *       long RS485 TX cannot sit on a full UAC RX FIFO.
     */
    void USB_App_Task(void);

    /** Re-arm the UAC endpoint from OTG_HS_IRQHandler (tud_task only). */
    void USB_App_TaskFromIsr(void);

    void USB_CDC_WriteStr(const char *s);

    uint32_t USB_App_RxMsgCount(void);
    uint32_t USB_App_RxByteCount(void);
    uint32_t USB_App_UacWindowCount(void);
    uint32_t USB_App_BadCount(void);
    /** Compatibility counters; direct transport reports routing faults in 2. */
    uint32_t USB_App_BadReasonCount(uint8_t reason);
    /** Legacy compatibility value; direct transport returns 0xFFFF. */
    uint16_t USB_App_LastPackSequence(void);
    void USB_App_StatsClear(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_APP_H */
