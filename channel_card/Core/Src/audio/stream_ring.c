/**
 ******************************************************************************
 * @file    stream_ring.c
 * @brief   Per-voice body FIFO filled from packed UAC2 BODY bursts.
 ******************************************************************************
 */

#include "stream_ring.h"
#include "usb_stream.h"

#include <stddef.h>
#include <string.h>

typedef struct
{
  volatile uint32_t wr;
  volatile uint32_t rd;
  volatile uint8_t consuming;
  uint8_t session;
  uint8_t replacement_armed;
  uint8_t body_published;
  uint16_t wave_id;
  uint32_t generation;
  volatile uint32_t release_rd;
  volatile uint32_t release_left;
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

static int16_t *StreamRing_DataAt(StreamRing_t *r, uint8_t voice,
                                  uint32_t index);

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
  r->generation++;
  r->wr = 0u;
  r->rd = 0u;
  r->consuming = 0u;
  r->session = 0xFFu;
  r->replacement_armed = 0u;
  r->body_published = 0u;
  r->wave_id = 0xFFFFu;
  r->release_rd = 0u;
  r->release_left = 0u;
}

void StreamRing_Prime(uint8_t voice)
{
  if (voice >= SAMPLE_VOICES)
  {
    return;
  }
  StreamRing_At(voice)->consuming = 1u;
}

uint32_t StreamRing_BeginReplacement(uint8_t voice, uint16_t wave_id,
                                     uint32_t release_samples)
{
  StreamRing_t *r;
  uint32_t keep;

  if (voice >= SAMPLE_VOICES)
  {
    return 0u;
  }
  r = StreamRing_At(voice);
  keep = StreamRing_Filled(r);
  if (keep > release_samples)
  {
    keep = release_samples;
  }

  /* Preserve [old rd, old rd + keep) for the release reader. The replacement
   * BODY has an empty logical FIFO whose origin is immediately after it. */
  r->generation++;
  r->release_rd = r->rd;
  r->release_left = keep;
  r->rd += keep;
  r->wr = r->rd;
  r->session = 0xFFu;
  r->replacement_armed = 1u;
  r->body_published = 0u;
  r->wave_id = wave_id;
  r->consuming = 1u;
  return keep;
}

int StreamRing_GetReleaseRel(uint8_t voice, uint32_t offset, int16_t *out)
{
  StreamRing_t *r;
  if (voice >= SAMPLE_VOICES || out == NULL)
  {
    return -1;
  }
  r = StreamRing_At(voice);
  if (offset >= r->release_left)
  {
    return -1;
  }
  *out = *StreamRing_DataAt(r, voice,
                            (r->release_rd + offset) % STREAM_RING_SAMPLES);
  return 0;
}

void StreamRing_AdvanceRelease(uint8_t voice, uint32_t n)
{
  StreamRing_t *r;
  if (voice >= SAMPLE_VOICES || n == 0u)
  {
    return;
  }
  r = StreamRing_At(voice);
  if (n > r->release_left)
  {
    n = r->release_left;
  }
  r->release_rd += n;
  r->release_left -= n;
}

uint32_t StreamRing_ReleaseLevel(uint8_t voice)
{
  return (voice < SAMPLE_VOICES) ? StreamRing_At(voice)->release_left : 0u;
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

int StreamRing_WriteBegin(uint8_t voice, uint8_t session, uint8_t sof,
                          uint16_t wave_id, uint32_t nsamp,
                          StreamRing_Write_t *write)
{
  StreamRing_t *r;

  if (voice >= SAMPLE_VOICES || nsamp == 0u ||
      nsamp > USB_STREAM_NSAMP_MAX || write == NULL)
  {
    return STREAM_RING_WRITE_ERROR;
  }
  memset(write, 0, sizeof *write);
  r = StreamRing_At(voice);
  if (wave_id != r->wave_id)
  {
    return STREAM_RING_WRITE_STALE;
  }
  if (sof != 0u && session != r->session)
  {
    /* Only nX may arm a new session and choose its replacement origin.
     * An old/unexpected SOF is transport-valid but cannot reset the ring. */
    if (r->replacement_armed == 0u)
    {
      return STREAM_RING_WRITE_STALE;
    }
    r->session = session;
    r->replacement_armed = 0u;
    s_sof_pkts++;
  }
  else if (sof == 0u && session != r->session)
  {
    /* A fresh vq authorized this refill, but note-off/new-note retired its
     * session before the ISO bytes arrived. It is valid transport data that
     * must be CRC-checked and ACKed, but it must not repopulate the ring. */
    return STREAM_RING_WRITE_STALE;
  }
  if (StreamRing_Filled(r) + r->release_left + nsamp > STREAM_RING_SAMPLES)
  {
    s_drop_pkts++;
    return STREAM_RING_WRITE_ERROR;
  }
  write->start_wr = r->wr;
  write->nsamp = nsamp;
  write->generation = r->generation;
  write->voice = voice;
  write->session = session;
  write->active = 1u;
  return STREAM_RING_WRITE_OK;
}

uint8_t StreamRing_WriteIsCurrent(const StreamRing_Write_t *write)
{
  StreamRing_t *r;
  if (write == NULL || write->active == 0u ||
      write->voice >= SAMPLE_VOICES)
  {
    return 0u;
  }
  r = StreamRing_At(write->voice);
  return (r->generation == write->generation &&
          r->session == write->session && r->wr == write->start_wr)
             ? 1u
             : 0u;
}

int16_t *StreamRing_WriteSpan(StreamRing_Write_t *write,
                              uint32_t *nsamp_out)
{
  StreamRing_t *r;
  uint32_t idx;
  uint32_t room;
  uint32_t remain;

  if (nsamp_out != NULL)
  {
    *nsamp_out = 0u;
  }
  if (write == NULL || write->active == 0u ||
      write->voice >= SAMPLE_VOICES || write->written >= write->nsamp)
  {
    return NULL;
  }
  r = StreamRing_At(write->voice);
  idx = (write->start_wr + write->written) % STREAM_RING_SAMPLES;
  room = (idx < STREAM_RING_BASE_SAMPLES)
             ? (STREAM_RING_BASE_SAMPLES - idx)
             : (STREAM_RING_SAMPLES - idx);
  remain = write->nsamp - write->written;
  if (room > remain)
  {
    room = remain;
  }
  if (nsamp_out != NULL)
  {
    *nsamp_out = room;
  }
  return StreamRing_DataAt(r, write->voice, idx);
}

int StreamRing_WriteAdvance(StreamRing_Write_t *write, uint32_t nsamp)
{
  if (write == NULL || write->active == 0u || nsamp == 0u ||
      nsamp > (write->nsamp - write->written))
  {
    return -1;
  }
  write->written += nsamp;
  return 0;
}

uint32_t StreamRing_WriteCommit(StreamRing_Write_t *write)
{
  StreamRing_t *r;
  uint32_t i;
  uint8_t all_zero = 1u;

  if (write == NULL || write->active == 0u ||
      write->voice >= SAMPLE_VOICES || write->written != write->nsamp)
  {
    return 0u;
  }
  r = StreamRing_At(write->voice);
  if (StreamRing_WriteIsCurrent(write) == 0u)
  {
    write->active = 0u;
    return 0u;
  }
  for (i = 0u; i < write->nsamp; i++)
  {
    uint32_t idx = (write->start_wr + i) % STREAM_RING_SAMPLES;
    if (*StreamRing_DataAt(r, write->voice, idx) != 0)
    {
      all_zero = 0u;
      break;
    }
  }
  r->wr = write->start_wr + write->nsamp;
  r->body_published = 1u;
  s_rx_pkts++;
  if (all_zero != 0u)
  {
    s_zero_pkts++;
  }
  write->active = 0u;
  return write->nsamp;
}

void StreamRing_WriteAbort(StreamRing_Write_t *write)
{
  if (write != NULL)
  {
    write->active = 0u;
  }
}

uint32_t StreamRing_WriteVoice(uint8_t voice, uint8_t session, uint8_t sof,
                               uint16_t wave_id, const int16_t *samples,
                               uint32_t nsamp)
{
  StreamRing_Write_t write;
  uint32_t copied = 0u;

  if (voice >= SAMPLE_VOICES || samples == NULL || nsamp == 0u ||
      nsamp > USB_STREAM_NSAMP_MAX)
  {
    return 0u;
  }
  if (StreamRing_WriteBegin(voice, session, sof, wave_id, nsamp, &write) != 0)
  {
    return 0u;
  }
  while (copied < nsamp)
  {
    uint32_t span_n = 0u;
    int16_t *span = StreamRing_WriteSpan(&write, &span_n);
    if (span == NULL || span_n == 0u)
    {
      StreamRing_WriteAbort(&write);
      return 0u;
    }
    memcpy(span, samples + copied, span_n * sizeof(int16_t));
    if (StreamRing_WriteAdvance(&write, span_n) != 0)
    {
      StreamRing_WriteAbort(&write);
      return 0u;
    }
    copied += span_n;
  }
  return StreamRing_WriteCommit(&write);
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

uint32_t StreamRing_FreeLevel(uint8_t voice)
{
  uint32_t used;
  if (voice >= SAMPLE_VOICES)
  {
    return 0u;
  }
  used = StreamRing_Filled(StreamRing_At(voice)) +
         StreamRing_At(voice)->release_left;
  return (used < STREAM_RING_SAMPLES) ? (STREAM_RING_SAMPLES - used) : 0u;
}

uint8_t StreamRing_HasBody(uint8_t voice)
{
  if (voice >= SAMPLE_VOICES)
  {
    return 0u;
  }
  return StreamRing_At(voice)->body_published;
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
