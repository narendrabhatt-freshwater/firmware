/**
 ******************************************************************************
 * @file    stream_ring.c
 * @brief   Per-voice dry int16 rings for UAC sustain (int16 → Q31 on pop).
 ******************************************************************************
 */

#include "stream_ring.h"

#include <stddef.h>

typedef struct
{
  volatile uint32_t wr;
  volatile uint32_t rd;
  volatile uint8_t consuming;
  int16_t data[STREAM_RING_SAMPLES];
} StreamRing_t;

static StreamRing_t s_rings[SAMPLE_VOICES];

/* Free-running wr/rd; capacity STREAM_RING_SAMPLES. */
static uint32_t StreamRing_Avail(const StreamRing_t *r)
{
  uint32_t wr = r->wr;
  uint32_t rd = r->rd;
  return wr - rd;
}

void StreamRing_Init(void)
{
  StreamRing_ResetAll();
}

void StreamRing_Reset(uint8_t voice)
{
  if (voice >= SAMPLE_VOICES)
  {
    return;
  }
  s_rings[voice].wr = 0u;
  s_rings[voice].rd = 0u;
  s_rings[voice].consuming = 0u;
}

void StreamRing_Prime(uint8_t voice)
{
  if (voice >= SAMPLE_VOICES)
  {
    return;
  }
  s_rings[voice].wr = 0u;
  s_rings[voice].rd = 0u;
  s_rings[voice].consuming = 1u;
}

void StreamRing_Release(uint8_t voice)
{
  if (voice >= SAMPLE_VOICES)
  {
    return;
  }
  s_rings[voice].consuming = 0u;
}

void StreamRing_ResetAll(void)
{
  uint8_t i;
  for (i = 0u; i < SAMPLE_VOICES; i++)
  {
    StreamRing_Reset(i);
  }
}

void StreamRing_WriteInterleaved(const int16_t *interleaved, uint32_t nframes)
{
  uint32_t f;
  uint8_t ch;

  if (interleaved == NULL || nframes == 0u)
  {
    return;
  }

  for (f = 0u; f < nframes; f++)
  {
    for (ch = 0u; ch < SAMPLE_VOICES; ch++)
    {
      StreamRing_t *r = &s_rings[ch];
      uint32_t avail = StreamRing_Avail(r);
      if (avail >= STREAM_RING_SAMPLES)
      {
        if (r->consuming != 0u)
        {
          /* Playhead is live: dropping rd would skip DAC samples. */
          continue;
        }
        /* Idle: drop oldest so the ring holds the most recent UAC. */
        r->rd++;
      }
      r->data[r->wr % STREAM_RING_SAMPLES] = interleaved[f * SAMPLE_VOICES + ch];
      r->wr++;
    }
  }
}

int32_t StreamRing_NextSample(uint8_t voice)
{
  StreamRing_t *r;
  int16_t s16;

  if (voice >= SAMPLE_VOICES)
  {
    return 0;
  }
  r = &s_rings[voice];
  if (StreamRing_Avail(r) == 0u)
  {
    return 0;
  }
  s16 = r->data[r->rd % STREAM_RING_SAMPLES];
  r->rd++;
  /* int16 → Q31: place in high half */
  return ((int32_t)s16) << 16;
}

uint32_t StreamRing_FillLevel(uint8_t voice)
{
  if (voice >= SAMPLE_VOICES)
  {
    return 0u;
  }
  {
    uint32_t a = StreamRing_Avail(&s_rings[voice]);
    return (a > STREAM_RING_SAMPLES) ? STREAM_RING_SAMPLES : a;
  }
}

uint8_t StreamRing_FillQuarters(uint8_t voice)
{
  uint32_t fill = StreamRing_FillLevel(voice);
  uint32_t q = (fill * 4u) / STREAM_RING_SAMPLES;
  if (q > 4u)
  {
    q = 4u;
  }
  return (uint8_t)q;
}
