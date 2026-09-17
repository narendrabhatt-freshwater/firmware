/* Binary CDC application layer. Call USB_App_Task from the main loop. */
#ifndef USB_APP_H
#define USB_APP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    void USB_App_Init(void);

    void USB_App_Task(void);

    void USB_CDC_WriteStr(const char *s);

    uint32_t USB_App_RxMsgCount(void);
    uint32_t USB_App_RxByteCount(void);
    uint32_t USB_App_BlockCount(void);
    uint32_t USB_App_BadCount(void);
    /** Transport faults are counted in reason 4. */
    uint32_t USB_App_BadReasonCount(uint8_t reason);
    /** Last processed BODY sequence. */
    uint16_t USB_App_LastPackSequence(void);
    void USB_App_StatsClear(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_APP_H */
