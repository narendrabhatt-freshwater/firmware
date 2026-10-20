#pragma once
#include "main.h"
struct DMA_Stream_TypeDef { uint32_t CR, NDTR; uintptr_t M0AR, M1AR; };
struct DMA_HandleTypeDef {
    DMA_Stream_TypeDef *Instance;
    void *Parent;
    void (*XferCpltCallback)(DMA_HandleTypeDef *);
    void (*XferM1CpltCallback)(DMA_HandleTypeDef *);
    void (*XferHalfCpltCallback)(DMA_HandleTypeDef *);
    void (*XferM1HalfCpltCallback)(DMA_HandleTypeDef *);
    void (*XferErrorCallback)(DMA_HandleTypeDef *);
};
struct I2S_HandleTypeDef {
    TestSpi *Instance;
    DMA_HandleTypeDef *hdmatx;
    const uint16_t *pTxBuffPtr;
    uint16_t TxXferSize, TxXferCount, RxXferCount;
    uint32_t ErrorCode, State;
};
extern I2S_HandleTypeDef hi2s1, hi2s2;
enum HAL_StatusTypeDef { HAL_OK, HAL_ERROR };
enum HAL_DMA_MemoryTypeDef { MEMORY0, MEMORY1 };
enum { HAL_I2S_ERROR_NONE, HAL_I2S_ERROR_DMA, HAL_I2S_STATE_READY, HAL_I2S_STATE_BUSY_TX };
HAL_StatusTypeDef HAL_DMAEx_MultiBufferStart_IT(DMA_HandleTypeDef *, uintptr_t, uintptr_t, uintptr_t, uint32_t);
HAL_StatusTypeDef HAL_DMAEx_ChangeMemory(DMA_HandleTypeDef *, uintptr_t, HAL_DMA_MemoryTypeDef);
void HAL_I2S_ErrorCallback(I2S_HandleTypeDef *);
