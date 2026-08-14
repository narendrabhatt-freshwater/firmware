/**
 ******************************************************************************
 * @file    stream_ring.c
 * @brief   Per-voice body FIFO filled from tagged UAC frames.
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
  uint8_t session;
  int16_t data[STREAM_RING_SAMPLES];
} StreamRing_t;

/* .bss is DTCM on this part — CPU fill from USB, mix reads in the I2S path. */
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
  s_rings[voice].session = 0xFFu;
}

void StreamRing_Prime(uint8_t voice)
{
  if (voice >= SAMPLE_VOICES)
  {
    return;
  }
  s_rings[voice].consuming = 1u;
}

void StreamRing_Release(uint8_t voice)
{
  StreamRing_Reset(voice);
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
  if (StreamRing_Filled(r) >= STREAM_RING_SAMPLES)
  {
    /* Producer never overwrites unread samples. */
    return;
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
    const int16_t *fr = &interleaved[f * STREAM_UAC_CHANNELS];
    uint16_t tag = (uint16_t)fr[0];
    uint8_t route;
    uint8_t voice;
    uint8_t session;
    StreamRing_t *r;

    if (tag < STREAM_UAC_TAG_BASE)
    {
      continue;
    }
    route = (uint8_t)tag;
    if (route == STREAM_UAC_IDLE)
    {
      continue;
    }
    voice = (uint8_t)(route & 7u);
    session = (uint8_t)((route >> STREAM_UAC_SESSION_SHIFT) &
                        STREAM_UAC_SESSION_MASK);
    r = &s_rings[voice];
    if ((route & STREAM_UAC_SOF) != 0u && session != r->session)
    {
      r->wr = 0u;
      r->rd = 0u;
      r->session = session;
      NoteBank_OnBodySof(voice);
    }
    for (i = 1u; i < STREAM_UAC_CHANNELS; i++)
    {
      StreamRing_Push(r, fr[i]);
    }
  }
}

int StreamRing_GetRel(uint8_t voice, uint32_t offset, int16_t *out)
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
  if (offset >= (wr - rd))
  {
    return -1;
  }
  *out = r->data[(rd + offset) % STREAM_RING_SAMPLES];
  return 0;
}

void StreamRing_Advance(uint8_t voice, uint32_t n)
{
  StreamRing_t *r;
  uint32_t filled;

  if (voice >= SAMPLE_VOICES || n == 0u)
  {
    return;
  }
  r = &s_rings[voice];
  filled = StreamRing_Filled(r);
  if (n > filled)
  {
    n = filled;
  }
  r->rd += n;
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
