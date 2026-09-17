#ifndef USB_TEST_HAL_H
#define USB_TEST_HAL_H
#include <stdint.h>
#define HAL_OK 0
#define HAL_ERROR 1
#define OTG_HS_IRQn 77
#define RCC_PERIPHCLK_USB 1
#define RCC_PLL3VCIRANGE_2 2
#define RCC_PLL3VCOWIDE 3
#define RCC_USBCLKSOURCE_PLL3 4
#define EP_TYPE_CTRL 0
#define EP_TYPE_BULK 2
#define EP_TYPE_INTR 3
#define __DMB() __asm__ volatile("" ::: "memory")
typedef int HAL_StatusTypeDef;
typedef struct { uint8_t is_stall; } PCD_EPTypeDef;
typedef struct { uint32_t Setup[12]; PCD_EPTypeDef IN_ep[16], OUT_ep[16]; } PCD_HandleTypeDef;
typedef struct {
  uint32_t PeriphClockSelection,UsbClockSelection;
  struct { uint32_t PLL3M,PLL3N,PLL3P,PLL3Q,PLL3R,PLL3RGE,PLL3VCOSEL,PLL3FRACN; } PLL3;
} RCC_PeriphCLKInitTypeDef;
uint32_t HAL_GetTick(void);
uint32_t HAL_GetUIDw0(void);
uint32_t HAL_GetUIDw1(void);
uint32_t HAL_GetUIDw2(void);
int HAL_RCCEx_PeriphCLKConfig(RCC_PeriphCLKInitTypeDef *p);
void HAL_NVIC_DisableIRQ(int irq);
void HAL_NVIC_EnableIRQ(int irq);
int HAL_PCD_EP_Transmit(PCD_HandleTypeDef*,uint8_t,uint8_t*,uint32_t);
int HAL_PCD_EP_Receive(PCD_HandleTypeDef*,uint8_t,uint8_t*,uint32_t);
int HAL_PCD_EP_Open(PCD_HandleTypeDef*,uint8_t,uint16_t,uint8_t);
int HAL_PCD_EP_Close(PCD_HandleTypeDef*,uint8_t);
int HAL_PCD_EP_SetStall(PCD_HandleTypeDef*,uint8_t);
int HAL_PCD_EP_ClrStall(PCD_HandleTypeDef*,uint8_t);
int HAL_PCD_SetAddress(PCD_HandleTypeDef*,uint8_t);
int HAL_PCDEx_SetRxFiFo(PCD_HandleTypeDef*,uint16_t);
int HAL_PCDEx_SetTxFiFo(PCD_HandleTypeDef*,uint8_t,uint16_t);
int HAL_PCD_Start(PCD_HandleTypeDef*);
uint32_t HAL_PCD_EP_GetRxCount(PCD_HandleTypeDef*,uint8_t);
void HAL_PCD_ResetCallback(PCD_HandleTypeDef*);
void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef*);
void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef*,uint8_t);
void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef*,uint8_t);
void HAL_PCD_SuspendCallback(PCD_HandleTypeDef*);
void HAL_PCD_ResumeCallback(PCD_HandleTypeDef*);
#endif

int HAL_PCD_EP_Abort(PCD_HandleTypeDef *p,uint8_t ep);
int HAL_PCD_EP_Flush(PCD_HandleTypeDef *p,uint8_t ep);
