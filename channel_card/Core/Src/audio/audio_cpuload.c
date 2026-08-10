/**
 ******************************************************************************
 * @file    audio_cpuload.c
 * @brief   CPU-load probe on LED_Y (PB9) for Channel Card NoteBank fill.
 *
 * Extracted from audio_bridge.c (behavior-preserving). LED duty cycle and
 * queue-full refill rule are unchanged.
 ******************************************************************************
 */

#include "audio_cpuload.h"

#include "main.h"
#include "note_bank.h"

/*
 * CPU load probe on LED_Y (PB9):
 *   low  = busy generating NoteBank samples
 *   high = idle (waiting for DMA / queue drain)
 * Scope duty: CPU% ≈ t_busy / (t_busy + t_idle).
 *
 * Queue mode: soft ring drained by DMA half/full callbacks (96 frames).
 * Capacity is larger than one half so the main-loop producer can stay ahead.
 * LED is idle only while the queue is full; otherwise Poll fills to full
 * (same duty-cycle idea as a ~16-sample low watermark with sample-at-a-time
 * drain — DMA gulps a half at once, so "not full → fill" is the safe rule).
 */
#define CPULOAD_Q_SIZE 256u
/* Classic sample-at-a-time low watermark is ~16; with DMA half gulps of
 * AUDIO_I2S_BUF_FRAMES/2 (96) we refill whenever the queue is not full instead. */

static volatile Audio_CpuLoadMode_t cpuload_mode = AUDIO_CPULOAD_OFF;

/* SPSC soft queue for AUDIO_CPULOAD_QUEUE (main produces, DMA consumes). */
static int32_t cpuload_q[CPULOAD_Q_SIZE];
static volatile uint32_t cpuload_q_wr = 0;
static volatile uint32_t cpuload_q_rd = 0;

void Audio_CpuLoad_LedBusy(uint8_t busy)
{
  HAL_GPIO_WritePin(LED_Y_GPIO_Port, LED_Y_Pin,
                    busy ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static uint32_t CpuLoad_QueueCount(void)
{
  uint32_t wr = cpuload_q_wr;
  uint32_t rd = cpuload_q_rd;
  if (wr >= rd)
  {
    return wr - rd;
  }
  return CPULOAD_Q_SIZE - rd + wr;
}

static void CpuLoad_QueueReset(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  cpuload_q_wr = 0;
  cpuload_q_rd = 0;
  if (!primask)
  {
    __enable_irq();
  }
}

static void CpuLoad_QueuePush(int32_t s)
{
  uint32_t next = (cpuload_q_wr + 1u) % CPULOAD_Q_SIZE;
  if (next == cpuload_q_rd)
  {
    return; /* full — should not happen if producer respects FULL-1 */
  }
  cpuload_q[cpuload_q_wr] = s;
  cpuload_q_wr = next;
}

int32_t Audio_CpuLoad_QueuePop(void)
{
  if (cpuload_q_wr == cpuload_q_rd)
  {
    return 0;
  }
  int32_t s = cpuload_q[cpuload_q_rd];
  cpuload_q_rd = (cpuload_q_rd + 1u) % CPULOAD_Q_SIZE;
  return s;
}

static void CpuLoad_QueuePrefill(void)
{
  while (CpuLoad_QueueCount() < (CPULOAD_Q_SIZE - 1u))
  {
    if (NoteBank_AnyActive())
    {
      CpuLoad_QueuePush(NoteBank_NextSample());
    }
    else
    {
      CpuLoad_QueuePush(0);
    }
  }
}

void Audio_CpuLoad_SetMode(Audio_CpuLoadMode_t mode)
{
  if (mode > AUDIO_CPULOAD_QUEUE)
  {
    mode = AUDIO_CPULOAD_OFF;
  }
  CpuLoad_QueueReset();
  cpuload_mode = mode;
  if (mode == AUDIO_CPULOAD_OFF)
  {
    HAL_GPIO_WritePin(LED_Y_GPIO_Port, LED_Y_Pin, GPIO_PIN_RESET);
  }
  else
  {
    if (mode == AUDIO_CPULOAD_QUEUE)
    {
      CpuLoad_QueuePrefill();
    }
    Audio_CpuLoad_LedBusy(0); /* idle high until next fill burst */
  }
}

Audio_CpuLoadMode_t Audio_CpuLoad_GetMode(void) { return cpuload_mode; }

uint8_t Audio_CpuLoad_IsActive(void)
{
  return (cpuload_mode != AUDIO_CPULOAD_OFF) ? 1u : 0u;
}

void Audio_CpuLoad_Poll(void)
{
  uint32_t count;

  if (cpuload_mode != AUDIO_CPULOAD_QUEUE)
  {
    return;
  }

  count = CpuLoad_QueueCount();
  /* Full → idle (LED high). Refill whenever not full so a 48-frame DMA
   * pop cannot underrun before the next main-loop Poll. */
  if (count >= (CPULOAD_Q_SIZE - 1u))
  {
    Audio_CpuLoad_LedBusy(0);
    return;
  }

  Audio_CpuLoad_LedBusy(1);
  while (CpuLoad_QueueCount() < (CPULOAD_Q_SIZE - 1u))
  {
    if (!NoteBank_AnyActive())
    {
      CpuLoad_QueuePush(0);
    }
    else
    {
      CpuLoad_QueuePush(NoteBank_NextSample());
    }
  }
  Audio_CpuLoad_LedBusy(0);
}
