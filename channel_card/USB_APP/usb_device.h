#ifndef CHANNEL_USB_DEVICE_H
#define CHANNEL_USB_DEVICE_H
#include <stdint.h>
void USB_Device_Init(void);
uint32_t USB_Device_Read(uint8_t *dst, uint32_t size);
uint8_t USB_Device_Write(const uint8_t *src, uint32_t size);
uint32_t USB_Device_TxFree(void);
uint32_t USB_Device_Epoch(void);
uint8_t USB_Device_Connected(void);
void USB_Device_Fault(void);
void USB_Device_Debug(char *text, uint32_t size);
const uint8_t *USB_Descriptor(uint8_t type, uint8_t index, uint16_t *size);
#endif
