/**
 ******************************************************************************
 * @file    uart5_rx.h
 * @brief   Interrupt-driven receive ring buffer for UART5 (RS485 console).
 ******************************************************************************
 */

#ifndef __UART5_RX_H__
#define __UART5_RX_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /** Arm the UART5 RX interrupt. Call once after MX_UART5_Init(). */
  void Uart5Rx_Init(void);

  /** Pop one buffered byte. Returns 1 and writes *out if a byte was
   * available, 0 if the buffer is empty. */
  uint8_t Uart5Rx_Get(uint8_t *out);

  /** Bytes lost since boot to hardware overrun or a full ring buffer.
   * Non-zero means a console line was corrupted — never ignore it. */
  uint32_t Uart5Rx_DroppedCount(void);

#ifdef __cplusplus
}
#endif

#endif /* __UART5_RX_H__ */
