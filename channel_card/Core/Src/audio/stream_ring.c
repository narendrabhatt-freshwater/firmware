/**
 ******************************************************************************
 * @file    stream_ring.c
 * @brief   Per-voice body FIFO filled from packed UAC2 BODY bursts.
 ******************************************************************************
 */

#include "stream_ring.h"
#include "usb_stream.h"

#if defined(__arm__) || defined(__thumb__)
#include "main.h"
#endif

#include <stddef.h>
#include <string.h>

#define STREAM_RING_STORAGE_SAMPLES STREAM_RING_SAMPLES

typedef struct
{
  /* Monotonic logical boundaries mapped into physical storage by modulo:
   * no pending: current=[rd, wr)
   * pending:    current=[rd, split), pending=[split, wr) */
  volatile uint32_t rd;
  volatile uint32_t split;
  volatile uint32_t wr;
  volatile uint8_t consuming;
  uint8_t current_session;
  volatile uint8_t pending_armed;
  volatile uint8_t pending_session;
  uint16_t current_wave_id;
  volatile uint16_t pending_wave_id;
  uint32_t generation;
  int8_t data[STREAM_RING_STORAGE_SAMPLES];
} StreamRing_t;

/* CPU-only producer/consumer storage. The default .bss placement keeps all
 * eight complete rings contiguous in DTCM; USB DMA never accesses it. */
static StreamRing_t s_rings[SAMPLE_VOICES] __attribute__((aligned(32)));

static StreamRing_t *StreamRing_At(uint8_t voice)
{
  return &s_rings[voice];
}
static volatile uint32_t s_drop_pkts;
static volatile uint32_t s_rx_pkts;
static volatile uint32_t s_sof_pkts;
static volatile uint32_t s_zero_pkts;
static volatile uint32_t s_stale_pkts;
static volatile uint32_t s_future_pkts;
static volatile uint32_t s_full_pkts;
static volatile uint32_t s_superseded_pkts;
static volatile uint16_t s_last_uac_sequence;
/* 0xFFFFFFFF = no consume sample since last clear. */
static volatile uint32_t s_min_fill = 0xFFFFFFFFu;

static int8_t *StreamRing_DataAt(StreamRing_t *r, uint32_t index);

static uint32_t StreamRing_CurrentFilled(const StreamRing_t *r)
{
  return (r->pending_armed != 0u ? r->split : r->wr) - r->rd;
}

static uint32_t StreamRing_PendingFilled(const StreamRing_t *r)
{
  return r->pending_armed != 0u ? r->wr - r->split : 0u;
}

void StreamRing_Init(void)
{
  s_last_uac_sequence = 0u;
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
  r->rd = 0u;
  r->split = 0u;
  r->wr = 0u;
  r->consuming = 0u;
  r->current_session = 0xFFu;
  r->pending_armed = 0u;
  r->pending_session = 0xFFu;
  r->current_wave_id = 0xFFFFu;
  r->pending_wave_id = 0xFFFFu;
}

void StreamRing_Prime(uint8_t voice)
{
  if (voice >= SAMPLE_VOICES)
  {
    return;
  }
  StreamRing_At(voice)->consuming = 1u;
}

void StreamRing_ArmPending(uint8_t voice, uint16_t wave_id, uint8_t session)
{
  StreamRing_t *r;
  if (voice >= SAMPLE_VOICES)
  {
    return;
  }
  r = StreamRing_At(voice);
  /* Retire only the prior pending producer. split stays frozen. */
  r->generation++;
  if (r->pending_armed != 0u)
  {
    r->wr = r->split;
  }
  else
  {
    r->split = r->wr;
  }
  r->pending_wave_id = wave_id;
  r->pending_session = session;
  r->pending_armed = 1u;
}

int StreamRing_StartNote(uint8_t voice)
{
  StreamRing_t *r;
  if (voice >= SAMPLE_VOICES) return -1;
  r = StreamRing_At(voice);
  if (r->pending_armed == 0u ||
      StreamRing_PendingFilled(r) < USB_STREAM_UAC_BODY_SAMPLES) return -1;
  r->generation++;
  r->rd = r->split;
  r->current_session = r->pending_session;
  r->current_wave_id = r->pending_wave_id;
  r->pending_armed = 0u;
  r->split = r->wr;
  r->consuming = 1u;
  return 0;
}

void StreamRing_DiscardPending(uint8_t voice)
{
  StreamRing_t *r;
  if (voice >= SAMPLE_VOICES) return;
  r = StreamRing_At(voice);
  r->generation++;
  if (r->pending_armed != 0u)
  {
    r->wr = r->split;
  }
  r->pending_armed = 0u;
  r->split = r->wr;
  r->pending_session = 0xFFu;
  r->pending_wave_id = 0xFFFFu;
}

void StreamRing_EndCurrent(uint8_t voice)
{
  StreamRing_t *r;
  if (voice >= SAMPLE_VOICES) return;
  r = StreamRing_At(voice);
  r->generation++;
  r->rd = r->pending_armed != 0u ? r->split : r->wr;
  r->current_session = 0xFFu;
  r->current_wave_id = 0xFFFFu;
  r->consuming = 0u;
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

static int8_t *StreamRing_DataAt(StreamRing_t *r, uint32_t index)
{
  return &r->data[index];
}

int StreamRing_WriteBegin(uint8_t voice, uint8_t session, uint8_t sof,
                          uint16_t wave_id, uint32_t nsamp,
                          StreamRing_Write_t *write)
{
  StreamRing_t *r;
  uint8_t target_pending = 0u;

  if (voice >= SAMPLE_VOICES || nsamp == 0u ||
      nsamp > USB_STREAM_NSAMP_MAX || write == NULL)
  {
    return STREAM_RING_WRITE_ERROR;
  }
  memset(write, 0, sizeof *write);
  r = StreamRing_At(voice);
  if (sof != 0u)
  {
    if (r->pending_armed == 0u)
    {
      /* Repeated SOF remains valid after promotion until vq confirms it. */
      if (session == r->current_session && wave_id == r->current_wave_id)
      {
        sof = 0u;
      }
      else
      {
        s_future_pkts++;
        return STREAM_RING_WRITE_FUTURE;
      }
    }
    if (sof != 0u)
    {
      if (wave_id != r->pending_wave_id ||
          (r->pending_session != 0xFFu && session != r->pending_session))
      {
        s_superseded_pkts++;
        return STREAM_RING_WRITE_STALE;
      }
      r->pending_session = session;
      target_pending = 1u;
    }
  }
  if (sof == 0u && r->pending_armed != 0u &&
      wave_id == r->pending_wave_id && session == r->pending_session)
  {
    target_pending = 1u;
  }
  else if (sof == 0u && (wave_id != r->current_wave_id ||
                         r->pending_armed != 0u ||
                         session != r->current_session))
  {
    /* A fresh vq authorized this refill, but note-off/new-note invalidated its
     * session before the ISO bytes arrived. It is valid transport data, but
     * it must not repopulate the ring. */
    s_stale_pkts++;
    return STREAM_RING_WRITE_STALE;
  }
  if (StreamRing_CurrentFilled(r) + StreamRing_PendingFilled(r) + nsamp >
      STREAM_RING_SAMPLES)
  {
    s_drop_pkts++;
    s_full_pkts++;
#if defined(__arm__) || defined(__thumb__)
    /* A dropped BODY frame makes subsequent audio knowingly incorrect. */
    Error_Handler();
#endif
    return STREAM_RING_WRITE_ERROR;
  }
  write->pending = target_pending;
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
          ((write->pending != 0u && r->pending_armed != 0u &&
            r->pending_session == write->session &&
            r->wr == write->start_wr) ||
           (write->pending == 0u && r->pending_armed == 0u &&
            r->current_session == write->session &&
            r->wr == write->start_wr)))
             ? 1u
             : 0u;
}

int8_t *StreamRing_WriteSpan(StreamRing_Write_t *write,
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
  room = STREAM_RING_SAMPLES - idx;
  remain = write->nsamp - write->written;
  if (room > remain)
  {
    room = remain;
  }
  if (nsamp_out != NULL)
  {
    *nsamp_out = room;
  }
  return StreamRing_DataAt(r, idx);
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
    if (*StreamRing_DataAt(r, idx) != 0)
    {
      all_zero = 0u;
      break;
    }
  }
  r->wr = write->start_wr + write->nsamp;
  if (write->pending != 0u && write->sof != 0u) s_sof_pkts++;
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
                               uint16_t wave_id, const int8_t *samples,
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
    int8_t *span = StreamRing_WriteSpan(&write, &span_n);
    if (span == NULL || span_n == 0u)
    {
      StreamRing_WriteAbort(&write);
      return 0u;
    }
    memcpy(span, samples + copied, span_n * sizeof(int8_t));
    if (StreamRing_WriteAdvance(&write, span_n) != 0)
    {
      StreamRing_WriteAbort(&write);
      return 0u;
    }
    copied += span_n;
  }
  return StreamRing_WriteCommit(&write);
}

uint32_t StreamRing_WriteUac(const int8_t *packet)
{
  uint8_t tag;
  uint8_t voice;
  uint8_t session;
  uint8_t sof;
  uint16_t wave_id;
  uint16_t sequence;
  StreamRing_t *r;
  if (packet == NULL)
  {
    return 0u;
  }
  tag = (uint8_t)packet[0];
  if (tag == USB_STREAM_TAG_IDLE ||
      (tag & USB_STREAM_TAG_MASK) != USB_STREAM_TAG_BASE)
  {
    return 0u;
  }
  voice = (uint8_t)(tag & USB_STREAM_TAG_VOICE_MASK);
  if (voice >= SAMPLE_VOICES)
  {
    return 0u;
  }
  session = (uint8_t)packet[1];
  sequence = (uint16_t)(uint8_t)packet[2] |
             (uint16_t)((uint16_t)(uint8_t)packet[3] << 8u);
  /* vq snapshots this after every well-routed frame, whether its BODY was
   * accepted or rejected. The accompanying free count therefore describes
   * all frames through this sequence exactly. */
  s_last_uac_sequence = sequence;
  sof = (tag & USB_STREAM_TAG_SOF) != 0u ? 1u : 0u;
  r = StreamRing_At(voice);
  wave_id = (r->pending_armed != 0u &&
             (sof != 0u || session == r->pending_session))
                ? r->pending_wave_id
                : r->current_wave_id;
  return StreamRing_WriteVoice(voice, session, sof, wave_id,
                               packet + USB_STREAM_UAC_HEADER_BYTES,
                               USB_STREAM_UAC_BODY_SAMPLES) ==
                 USB_STREAM_UAC_BODY_SAMPLES
             ? 1u
             : 0u;
}

uint16_t StreamRing_LastUacSequence(void)
{
  return s_last_uac_sequence;
}

int StreamRing_GetRel(uint8_t voice, uint32_t offset, int8_t *out)
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
  wr = r->pending_armed != 0u ? r->split : r->wr;
  if (offset >= (wr - rd))
  {
    return -1;
  }
  *out = *StreamRing_DataAt(r, (rd + offset) % STREAM_RING_SAMPLES);
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
  filled = StreamRing_CurrentFilled(r);
  if (n > filled)
  {
    n = filled;
  }
  r->rd += n;
  StreamRing_ObserveFill(voice);
}

uint32_t StreamRing_FillLevel(uint8_t voice)
{
  return StreamRing_CurrentFill(voice);
}

uint32_t StreamRing_CurrentFill(uint8_t voice)
{
  if (voice >= SAMPLE_VOICES)
  {
    return 0u;
  }
  {
    uint32_t a = StreamRing_CurrentFilled(StreamRing_At(voice));
    return (a > STREAM_RING_SAMPLES) ? STREAM_RING_SAMPLES : a;
  }
}

uint32_t StreamRing_PendingFill(uint8_t voice)
{
  uint32_t a;
  if (voice >= SAMPLE_VOICES) return 0u;
  a = StreamRing_PendingFilled(StreamRing_At(voice));
  return a > STREAM_RING_SAMPLES ? STREAM_RING_SAMPLES : a;
}

uint32_t StreamRing_FreeLevel(uint8_t voice)
{
  uint32_t used;
  if (voice >= SAMPLE_VOICES)
  {
    return 0u;
  }
  used = StreamRing_CurrentFilled(StreamRing_At(voice)) +
         StreamRing_PendingFilled(StreamRing_At(voice));
  return (used < STREAM_RING_SAMPLES) ? (STREAM_RING_SAMPLES - used) : 0u;
}

uint8_t StreamRing_HasBody(uint8_t voice)
{
  if (voice >= SAMPLE_VOICES)
  {
    return 0u;
  }
  return StreamRing_PendingFill(voice) >= USB_STREAM_UAC_BODY_SAMPLES ? 1u : 0u;
}

uint8_t StreamRing_HasPending(uint8_t voice)
{
  return voice < SAMPLE_VOICES ? StreamRing_At(voice)->pending_armed : 0u;
}

uint8_t StreamRing_TargetSession(uint8_t voice)
{
  StreamRing_t *r;
  if (voice >= SAMPLE_VOICES) return 0xFFu;
  r = StreamRing_At(voice);
  return r->pending_armed != 0u ? r->pending_session : r->current_session;
}

uint32_t StreamRing_TargetFill(uint8_t voice)
{
  if (voice >= SAMPLE_VOICES) return 0u;
  return StreamRing_HasPending(voice) != 0u ? StreamRing_PendingFill(voice)
                                            : StreamRing_CurrentFill(voice);
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
  f = StreamRing_CurrentFilled(r);
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

uint32_t StreamRing_StaleCount(void) { return s_stale_pkts; }
uint32_t StreamRing_FutureCount(void) { return s_future_pkts; }
uint32_t StreamRing_FullCount(void) { return s_full_pkts; }
uint32_t StreamRing_SupersededCount(void) { return s_superseded_pkts; }

void StreamRing_StatsClear(void)
{
  s_drop_pkts = 0u;
  s_rx_pkts = 0u;
  s_sof_pkts = 0u;
  s_zero_pkts = 0u;
  s_stale_pkts = 0u;
  s_future_pkts = 0u;
  s_full_pkts = 0u;
  s_superseded_pkts = 0u;
  s_min_fill = 0xFFFFFFFFu;
}

void StreamRing_DropCountClear(void)
{
  StreamRing_StatsClear();
}
