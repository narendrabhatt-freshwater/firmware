#pragma once
#include "main.h"
struct UART_HandleTypeDef { struct { uint32_t BaudRate; } Init; };
inline UART_HandleTypeDef huart5{{921600}};
inline int HAL_UARTEx_SetRxFifoThreshold(UART_HandleTypeDef *, unsigned) { return HAL_OK; }
inline int HAL_UARTEx_EnableFifoMode(UART_HandleTypeDef *) { return HAL_OK; }
