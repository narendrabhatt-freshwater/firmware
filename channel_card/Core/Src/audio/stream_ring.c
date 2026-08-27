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
  volatile uint8_t replacement_state;
  volatile uint8_t expected_session;
  uint8_t body_published;
  uint16_t wave_id;
  volatile uint16_t expected_wave_id;
  uint32_t generation;
  volatile uint32_t release_left;
  int16_t data[STREAM_RING_BASE_SAMPLES];
} StreamRing_t;

#if defined(__APPLE__)
#define STREAM_RING_SECTION(name) __attribute__((aligned(32)))
#else
#define STREAM_RING_SECTION(name) \
  __attribute__((aligned(32), section(name)))
#endif

/* Use all three CPU SRAM domains for genuine per-voice jitter storage. These
 * buffers are single-core CPU data (USB main loop + I2S ISR), never DMA, so
 * no cache-maintenance boundary is introduced. */
static StreamRing_t s_rings_dtcm[5];
static StreamRing_t s_rings_d2[2]
    STREAM_RING_SECTION(".ring_d2");
static StreamRing_t s_rings_d3[1]
    STREAM_RING_SECTION(".ring_d3");
/* The H725's otherwise-unused 64 KiB ITCM is CPU-accessible zero-wait-state
 * RAM. One extension bank per voice increases real jitter tolerance without
 * delaying playback or changing the 48 kHz int16 stream. */
static int16_t s_ring_tail[SAMPLE_VOICES][STREAM_RING_TAIL_SAMPLES]
    STREAM_RING_SECTION(".ring_itcm");

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

#define STREAM_REPLACEMENT_NONE 0u
#define STREAM_REPLACEMENT_PENDING 1u
#define STREAM_REPLACEMENT_READY 2u

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
  r->replacement_state = STREAM_REPLACEMENT_NONE;
  r->expected_session = 0xFFu;
  r->body_published = 0u;
  r->wave_id = 0xFFFFu;
  r->expected_wave_id = 0xFFFFu;
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

void StreamRing_ArmReplacement(uint8_t voice, uint16_t wave_id,
                               uint8_t session)
{
  StreamRing_t *r;
  if (voice >= SAMPLE_VOICES)
  {
    return;
  }
  r = StreamRing_At(voice);
  /* Retire any USB reservation for the previous note immediately. The audio
   * reader may keep consuming that ring until the I2S command is applied. */
  r->generation++;
  r->expected_wave_id = wave_id;
  r->expected_session = session;
  r->replacement_state = STREAM_REPLACEMENT_PENDING;
}

void StreamRing_Disarm(uint8_t voice)
{
  StreamRing_t *r;
  if (voice >= SAMPLE_VOICES)
  {
    return;
  }
  r = StreamRing_At(voice);
  r->generation++;
  r->replacement_state = STREAM_REPLACEMENT_NONE;
  r->expected_session = 0xFFu;
  r->expected_wave_id = 0xFFFFu;
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
  /* Legacy callers do not arm in the command path. Preserve their wildcard
   * SOF behavior while session-tagged nX keeps its exact binding. */
  if (r->replacement_state != STREAM_REPLACEMENT_PENDING ||
      r->expected_wave_id != wave_id)
  {
    r->expected_wave_id = wave_id;
    r->expected_session = 0xFFu;
  }
  keep = StreamRing_Filled(r);
  if (keep > release_samples)
  {
    keep = release_samples;
  }

  /* Truncate the old FIFO to its release tail and append replacement BODY
   * after it. rd consumes the old tail without jumping; when release_left
   * reaches zero it already points at the replacement BODY origin. */
  r->generation++;
  r->release_left = keep;
  r->wr = r->rd + keep;
  r->session = 0xFFu;
  r->replacement_state = STREAM_REPLACEMENT_READY;
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
                            (r->rd + offset) % STREAM_RING_SAMPLES);
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
  r->rd += n;
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
  if (sof != 0u)
  {
    if (r->replacement_state == STREAM_REPLACEMENT_NONE)
    {
      /* Direct UAC keeps SOF asserted throughout urgent prefill. Once the
       * matching session owns the ring, later SOF-tagged frames append. */
      if (session == r->session && wave_id == r->wave_id)
      {
        sof = 0u;
      }
      else
      {
        return STREAM_RING_WRITE_FUTURE;
      }
    }
    if (sof != 0u)
    {
      if (wave_id != r->expected_wave_id ||
          (r->expected_session != 0xFFu && session != r->expected_session))
      {
        return STREAM_RING_WRITE_STALE;
      }
      /* nX is authoritative already, but I2S has not selected its ring
       * origin. The next tagged UAC frame retries one audio frame later. */
      if (r->replacement_state == STREAM_REPLACEMENT_PENDING)
      {
        return STREAM_RING_WRITE_PENDING;
      }
      if (r->replacement_state != STREAM_REPLACEMENT_READY ||
          wave_id != r->wave_id ||
          (r->session != 0xFFu && session != r->session))
      {
        return STREAM_RING_WRITE_STALE;
      }
      r->session = session;
    }
  }
  if (sof == 0u && (wave_id != r->wave_id ||
                    r->replacement_state != STREAM_REPLACEMENT_NONE ||
                    session != r->session))
  {
    /* A fresh vq authorized this refill, but note-off/new-note retired its
     * session before the ISO bytes arrived. It is valid transport data, but
     * it must not repopulate the ring. */
    return STREAM_RING_WRITE_STALE;
  }
  if (StreamRing_Filled(r) + nsamp > STREAM_RING_SAMPLES)
  {
    s_drop_pkts++;
    return STREAM_RING_WRITE_ERROR;
  }
  write->start_wr = r->wr;
  write->nsamp = nsamp;
  write->generation = r->generation;
  write->voice = voice;
  write->session = session;
  write->sof = sof != 0u ? 1u : 0u;
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
  if (write->sof != 0u)
  {
    r->replacement_state = STREAM_REPLACEMENT_NONE;
    r->expected_session = 0xFFu;
    r->expected_wave_id = 0xFFFFu;
    s_sof_pkts++;
  }
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

uint32_t StreamRing_WriteUac(const int16_t *interleaved, uint32_t nframes)
{
  uint32_t frame;
  uint32_t accepted = 0u;
  if (interleaved == NULL)
  {
    return 0u;
  }
  for (frame = 0u; frame < nframes; ++frame)
  {
    const int16_t *src = interleaved +
                         frame * USB_STREAM_UAC_CHANNELS;
    uint16_t tag = (uint16_t)src[0];
    uint8_t voice;
    uint8_t session;
    uint8_t sof;
    uint16_t wave_id;
    StreamRing_t *r;
    if (tag == USB_STREAM_TAG_IDLE ||
        (tag & USB_STREAM_TAG_MASK) != USB_STREAM_TAG_BASE)
    {
      continue;
    }
    voice = (uint8_t)(tag & USB_STREAM_TAG_VOICE_MASK);
    if (voice >= SAMPLE_VOICES)
    {
      continue;
    }
    session = (uint8_t)((tag >> USB_STREAM_TAG_SESSION_SHIFT) &
                        USB_STREAM_TAG_SESSION_MASK);
    sof = (tag & USB_STREAM_TAG_SOF) != 0u ? 1u : 0u;
    r = StreamRing_At(voice);
    wave_id = (sof != 0u &&
               r->replacement_state != STREAM_REPLACEMENT_NONE)
                  ? r->expected_wave_id
                  : r->wave_id;
    if (StreamRing_WriteVoice(voice, session, sof, wave_id, src + 1,
                              USB_STREAM_UAC_BODY_SAMPLES) ==
        USB_STREAM_UAC_BODY_SAMPLES)
    {
      accepted++;
    }
  }
  return accepted;
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
  used = StreamRing_Filled(StreamRing_At(voice));
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
