/**
 ******************************************************************************
 * @file    stream_ring.c
 * @brief   Per-voice body FIFO filled from vendor bulk BODY bursts.
 ******************************************************************************
 */

#include "stream_ring.h"
#include "usb_stream.h"

#include <stddef.h>
#include <string.h>

void NoteBank_OnBodySof(uint8_t voice);

typedef struct
{
  volatile uint32_t wr;
  volatile uint32_t rd;
  volatile uint8_t consuming;
  uint8_t session;
  int16_t data[STREAM_RING_BASE_SAMPLES];
} StreamRing_t;

/* Use all three CPU SRAM domains for genuine per-voice jitter storage. These
 * buffers are single-core CPU data (USB main loop + I2S ISR), never DMA, so
 * no cache-maintenance boundary is introduced. */
static StreamRing_t s_rings_dtcm[5];
static StreamRing_t s_rings_d2[2]
    __attribute__((aligned(32), section(".ring_d2")));
static StreamRing_t s_rings_d3[1]
    __attribute__((aligned(32), section(".ring_d3")));
/* The H725's otherwise-unused 64 KiB ITCM is CPU-accessible zero-wait-state
 * RAM. One extension bank per voice increases real jitter tolerance without
 * delaying playback or changing the 48 kHz int16 stream. */
static int16_t s_ring_tail[SAMPLE_VOICES][STREAM_RING_TAIL_SAMPLES]
    __attribute__((aligned(32), section(".ring_itcm")));

static StreamRing_t *StreamRing_At(uint8_t voice)
{
  if (voice < 5u)
  {
    return &s_rings_dtcm[voice];
  }
  if (voice < 7u)
  {
    return &s_rings_d2[voice - 5u];
  }
  return &s_rings_d3[0];
}
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
  StreamRing_t *r = StreamRing_At(voice);
  r->wr = 0u;
  r->rd = 0u;
  r->consuming = 0u;
  r->session = 0xFFu;
}

void StreamRing_Prime(uint8_t voice)
{
  if (voice >= SAMPLE_VOICES)
  {
    return;
  }
  StreamRing_At(voice)->consuming = 1u;
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

static int16_t *StreamRing_DataAt(StreamRing_t *r, uint8_t voice,
                                  uint32_t index)
{
  if (index < STREAM_RING_BASE_SAMPLES)
  {
    return &r->data[index];
  }
  return &s_ring_tail[voice][index - STREAM_RING_BASE_SAMPLES];
}

static void StreamRing_CopyIn(StreamRing_t *r, uint8_t voice,
                              const int16_t *s, uint32_t n)
{
  uint32_t idx = r->wr % STREAM_RING_SAMPLES;
  while (n != 0u)
  {
    uint32_t room = (idx < STREAM_RING_BASE_SAMPLES)
                        ? (STREAM_RING_BASE_SAMPLES - idx)
                        : (STREAM_RING_SAMPLES - idx);
    uint32_t chunk = (n < room) ? n : room;
    memcpy(StreamRing_DataAt(r, voice, idx), s, chunk * sizeof(int16_t));
    s += chunk;
    n -= chunk;
    idx += chunk;
    if (idx == STREAM_RING_SAMPLES)
    {
      idx = 0u;
    }
  }
}

uint32_t StreamRing_WriteVoice(uint8_t voice, uint8_t session, uint8_t sof,
                               const int16_t *samples, uint32_t nsamp)
{
  StreamRing_t *r;
  uint32_t i;
  uint8_t all_zero = 1u;

  if (voice >= SAMPLE_VOICES || samples == NULL || nsamp == 0u ||
      nsamp > USB_STREAM_NSAMP_MAX)
  {
    return 0u;
  }
  r = StreamRing_At(voice);
  if (sof != 0u && session != r->session)
  {
    r->wr = 0u;
    r->rd = 0u;
    r->session = session;
    s_sof_pkts++;
    NoteBank_OnBodySof(voice);
  }
  /* Whole burst or none. A partial push with the host cursor already
   * advanced is a hole in the wav. */
  if (StreamRing_Filled(r) + nsamp > STREAM_RING_SAMPLES)
  {
    s_drop_pkts++;
    return 0u;
  }
  for (i = 0u; i < nsamp; i++)
  {
    if (samples[i] != 0)
    {
      all_zero = 0u;
      break;
    }
  }
  StreamRing_CopyIn(r, voice, samples, nsamp);
  r->wr += nsamp;
  s_rx_pkts++;
  if (all_zero != 0u)
  {
    s_zero_pkts++;
  }
  return nsamp;
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
  r = StreamRing_At(voice);
  rd = r->rd;
  wr = r->wr;
  if (offset >= (wr - rd))
  {
    return -1;
  }
  *out = *StreamRing_DataAt(r, voice,
                            (rd + offset) % STREAM_RING_SAMPLES);
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
  r = StreamRing_At(voice);
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
    uint32_t a = StreamRing_Filled(StreamRing_At(voice));
    return (a > STREAM_RING_SAMPLES) ? STREAM_RING_SAMPLES : a;
  }
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
  StreamRing_t *r = StreamRing_At(voice);
  if (r->consuming == 0u)
  {
    return;
  }
  f = StreamRing_Filled(r);
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
