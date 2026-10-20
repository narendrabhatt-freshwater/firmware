#pragma once
#include "main.h"
#include <vector>
struct TestEndpoint { unsigned is_stall; };
struct PCD_HandleTypeDef { uint8_t Setup[8]; TestEndpoint IN_ep[16], OUT_ep[16]; };
inline PCD_HandleTypeDef hpcd_USB_OTG_HS;
struct RCC_PeriphCLKInitTypeDef {
    unsigned PeriphClockSelection, UsbClockSelection;
    struct { unsigned PLL3M, PLL3N, PLL3P, PLL3Q, PLL3R, PLL3RGE, PLL3VCOSEL; } PLL3;
};
enum { RCC_PERIPHCLK_USB, RCC_PLL3VCIRANGE_2, RCC_PLL3VCOWIDE, RCC_USBCLKSOURCE_PLL3,
       EP_TYPE_BULK, EP_TYPE_CTRL, EP_TYPE_INTR };
inline unsigned usb_rx_count;
inline std::vector<uint8_t> usb_wire;
inline uint32_t HAL_GetUIDw0() { return 0; }
inline uint32_t HAL_GetUIDw1() { return 1; }
inline uint32_t HAL_GetUIDw2() { return 2; }
inline int HAL_RCCEx_PeriphCLKConfig(RCC_PeriphCLKInitTypeDef *) { return HAL_OK; }
inline int HAL_PCDEx_SetRxFiFo(PCD_HandleTypeDef *, unsigned) { return HAL_OK; }
inline int HAL_PCDEx_SetTxFiFo(PCD_HandleTypeDef *, unsigned, unsigned) { return HAL_OK; }
inline int HAL_PCD_Start(PCD_HandleTypeDef *) { return HAL_OK; }
inline int HAL_PCD_SetAddress(PCD_HandleTypeDef *, unsigned) { return HAL_OK; }
inline int HAL_PCD_EP_SetStall(PCD_HandleTypeDef *, unsigned) { return HAL_OK; }
inline int HAL_PCD_EP_ClrStall(PCD_HandleTypeDef *, unsigned) { return HAL_OK; }
inline int HAL_PCD_EP_Open(PCD_HandleTypeDef *, unsigned, unsigned, unsigned) { return HAL_OK; }
inline int HAL_PCD_EP_Close(PCD_HandleTypeDef *, unsigned) { return HAL_OK; }
inline int HAL_PCD_EP_Abort(PCD_HandleTypeDef *, unsigned) { return HAL_OK; }
inline int HAL_PCD_EP_Flush(PCD_HandleTypeDef *, unsigned) { return HAL_OK; }
inline int HAL_PCD_EP_Receive(PCD_HandleTypeDef *, unsigned ep, uint8_t *, unsigned) {
    if (ep == 1) usb_rx_count = 0;
    return HAL_OK;
}
inline unsigned HAL_PCD_EP_GetRxCount(PCD_HandleTypeDef *, unsigned) { return usb_rx_count; }
inline int HAL_PCD_EP_Transmit(PCD_HandleTypeDef *, unsigned ep, uint8_t *data, unsigned size) {
    if (ep == 0x81 && size) usb_wire.insert(usb_wire.end(), data, data + size);
    return HAL_OK;
}
