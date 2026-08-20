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
static volatile uint32_t s_drop_pkts;
static volatile uint32_t s_rx_pkts;
static volatile uint32_t s_sof_pkts;
static volatile uint32_t s_zero_pkts;
/* 0xFFFFFFFF = no consume sample since last clear. */
static volatile uint32_t s_min_fill = 0xFFFFFFFFu;

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
      s_sof_pkts++;
      NoteBank_OnBodySof(voice);
    }
    /* Whole packet or none. A partial push with the host cursor already
     * advanced is a hole in the wav — that is the scope amp-dip. */
    if (StreamRing_Filled(r) + (STREAM_UAC_CHANNELS - 1u) >
        STREAM_RING_SAMPLES)
    {
      s_drop_pkts++;
      continue;
    }
    {
      uint8_t all_zero = 1u;
      for (i = 1u; i < STREAM_UAC_CHANNELS; i++)
      {
        if (fr[i] != 0)
        {
          all_zero = 0u;
        }
        StreamRing_Push(r, fr[i]);
      }
      s_rx_pkts++;
      if (all_zero != 0u)
      {
        s_zero_pkts++;
      }
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
  StreamRing_ObserveFill(voice);
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
  if (filled == 0u)
  {
    return STREAM_SLOT_EMPTY;
  }
  if (slots > STREAM_SLOT_MAX)
  {
    slots = STREAM_SLOT_MAX;
  }
  return (uint8_t)slots;
}

uint32_t StreamRing_MaxFill(void)
{
  uint32_t max_fill = 0u;
  uint8_t i;
  for (i = 0u; i < SAMPLE_VOICES; i++)
  {
    uint32_t f = StreamRing_FillLevel(i);
    if (f > max_fill)
    {
      max_fill = f;
    }
  }
  return max_fill;
}

void StreamRing_ObserveFill(uint8_t voice)
{
  uint32_t f;

  if (voice >= SAMPLE_VOICES)
  {
    return;
  }
  if (s_rings[voice].consuming == 0u)
  {
    return;
  }
  f = StreamRing_Filled(&s_rings[voice]);
  if (f < s_min_fill)
  {
    s_min_fill = f;
  }
}

uint32_t StreamRing_MinFill(void)
{
  return s_min_fill;
}

uint32_t StreamRing_DropCount(void)
{
  return s_drop_pkts;
}

uint32_t StreamRing_RxCount(void)
{
  return s_rx_pkts;
}

uint32_t StreamRing_SofCount(void)
{
  return s_sof_pkts;
}

uint32_t StreamRing_ZeroCount(void)
{
  return s_zero_pkts;
}

void StreamRing_StatsClear(void)
{
  s_drop_pkts = 0u;
  s_rx_pkts = 0u;
  s_sof_pkts = 0u;
  s_zero_pkts = 0u;
  s_min_fill = 0xFFFFFFFFu;
}

void StreamRing_DropCountClear(void)
{
  StreamRing_StatsClear();
}
