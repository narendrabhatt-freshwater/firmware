/**
 ******************************************************************************
 * @file    audio_cpuload.h
 * @brief   CPU-load probe on LED_Y (PB9) for Channel Card NoteBank fill.
 *
 * Modes: OFF (LED free for chaser), DMA (busy around the I2S DMA callback
 * refill), QUEUE (soft ring produced in main, drained by the callback refill).
 ******************************************************************************
 */

#ifndef __AUDIO_CPULOAD_H__
#define __AUDIO_CPULOAD_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

  typedef enum
  {
    AUDIO_CPULOAD_OFF = 0,   /**< Normal path; LED_Y free for chaser */
    AUDIO_CPULOAD_DMA = 1,   /**< Busy=low around NoteBank DMA callback refill */
    AUDIO_CPULOAD_QUEUE = 2, /**< Soft queue (256) filled in main, drained by callback */
  } Audio_CpuLoadMode_t;

  /**
   * @brief Select CPU-load probe mode (console `cpuload`).
   * @param mode OFF / DMA / QUEUE.
   */
  void Audio_CpuLoad_SetMode(Audio_CpuLoadMode_t mode);

  /** @brief Current CPU-load probe mode. */
  Audio_CpuLoadMode_t Audio_CpuLoad_GetMode(void);

  /**
   * @brief Non-zero while DMA or queue probe owns LED_Y (pause LED chaser).
   */
  uint8_t Audio_CpuLoad_IsActive(void);

  /**
   * @brief Queue-mode producer; call from main loop.
   * @note No-op unless AUDIO_CPULOAD_QUEUE.
   */
  void Audio_CpuLoad_Poll(void);

  /**
   * @brief Drive LED_Y for scope duty-cycle (low = busy, high = idle).
   * @param busy Non-zero → LED low (busy); zero → LED high (idle).
   * @note Used by note-bank fill in audio_bridge when mode is DMA.
   */
  void Audio_CpuLoad_LedBusy(uint8_t busy);

  /**
   * @brief Pop one sample from the queue-mode soft ring (0 if empty).
   * @note Used by note-bank fill when mode is QUEUE.
   */
  int32_t Audio_CpuLoad_QueuePop(void);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_CPULOAD_H__ */
