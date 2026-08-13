/**
 ******************************************************************************
 * @file    stream_ring.c
 * @brief   Per-voice body rings filled from tagged UAC frames.
 ******************************************************************************
 */

#include "stream_ring.h"

#include <stddef.h>

void NoteBank_OnBodySof(uint8_t voice);

typedef struct
{
  volatile uint32_t wr;
  volatile uint32_t rd;
  volatile uint8_t consuming;
  int16_t data[STREAM_RING_SAMPLES];
} StreamRing_t;

static StreamRing_t s_rings[SAMPLE_VOICES];

static uint32_t StreamRing_Filled(const StreamRing_t *r)
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
  /* Prefill must survive nX — SOF already reset wr/rd for this body. */
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

static void StreamRing_Push(StreamRing_t *r, int16_t s)
{
  uint32_t filled = StreamRing_Filled(r);
  if (filled >= STREAM_RING_SAMPLES)
  {
    if (r->consuming != 0u)
    {
      return;
    }
    r->rd++;
  }
  r->data[r->wr % STREAM_RING_SAMPLES] = s;
  r->wr++;
}

void StreamRing_WriteInterleaved(const int16_t *interleaved, uint32_t nframes)
{
  uint32_t f;
  uint8_t i;

  if (interleaved == NULL || nframes == 0u)
  {
    return;
  }

  for (f = 0u; f < nframes; f++)
  {
    const int16_t *fr = &interleaved[f * SAMPLE_VOICES];
    uint16_t tag = (uint16_t)fr[0];
    uint8_t route;
    uint8_t voice;
    StreamRing_t *r;

    /* Full-scale tags only. 0 / mute / junk must not demux as voice 0. */
    if (tag < STREAM_UAC_TAG_BASE)
    {
      continue;
    }
    route = (uint8_t)(tag & 0x1Fu);
    voice = (uint8_t)(route & 7u);
    r = &s_rings[voice];
    if ((route & STREAM_UAC_SOF) != 0u)
    {
      r->wr = 0u;
      r->rd = 0u;
      NoteBank_OnBodySof(voice);
    }
    for (i = 1u; i < SAMPLE_VOICES; i++)
    {
      StreamRing_Push(r, fr[i]);
    }
  }
}

int StreamRing_Get(uint8_t voice, uint32_t body_idx, int16_t *out)
{
  StreamRing_t *r;
  uint32_t rd;
  uint32_t wr;

  if (voice >= SAMPLE_VOICES || out == NULL)
  {
    return -1;
  }
  r = &s_rings[voice];
  rd = r->rd;
  wr = r->wr;
  if (body_idx < rd || body_idx >= wr)
  {
    return -1;
  }
  *out = r->data[body_idx % STREAM_RING_SAMPLES];
  return 0;
}

void StreamRing_DropBefore(uint8_t voice, uint32_t body_idx)
{
  StreamRing_t *r;
  uint32_t wr;

  if (voice >= SAMPLE_VOICES)
  {
    return;
  }
  r = &s_rings[voice];
  wr = r->wr;
  if (body_idx > wr)
  {
    body_idx = wr;
  }
  if (body_idx > r->rd)
  {
    r->rd = body_idx;
  }
}

uint32_t StreamRing_FillLevel(uint8_t voice)
{
  if (voice >= SAMPLE_VOICES)
  {
    return 0u;
  }
  {
    uint32_t a = StreamRing_Filled(&s_rings[voice]);
    return (a > STREAM_RING_SAMPLES) ? STREAM_RING_SAMPLES : a;
  }
}

uint8_t StreamRing_FreeSlots(uint8_t voice)
{
  uint32_t filled = StreamRing_FillLevel(voice);
  uint32_t free_samp = STREAM_RING_SAMPLES - filled;
  uint32_t slots = free_samp / STREAM_SLOT_LEN;
  if (slots > STREAM_SLOTS)
  {
    slots = STREAM_SLOTS;
  }
  return (uint8_t)slots;
}
