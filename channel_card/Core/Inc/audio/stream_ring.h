/**
 ******************************************************************************
 * @file    stream_ring.h
 * @brief   Per-voice body FIFO: 3 × 4080 int16, filled from USB.
 *
 * SPSC: USB writes from main, the playhead reads in I2S. A full FIFO drops
 * the write (never unread samples). An empty FIFO is an underrun.
 *
 * BODY burst: voice + session + SOF + packed int16 samples. Note-on arms the
 * replacement origin; only its matching wave/session SOF may start the body.
 * Repeated bursts append and stale/unarmed sessions never reset the FIFO.
 ******************************************************************************
 */

#ifndef __STREAM_RING_H__
#define __STREAM_RING_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

#include "attack_bank.h" /* SAMPLE_VOICES, SAMPLE_CROSSFADE_LEN */

/** Two base banks live in DTCM/D2/D3 and one extension bank lives in ITCM. */
#define STREAM_BANK_LEN 4080u
#define STREAM_BASE_BANKS 2u
#define STREAM_TAIL_BANKS 1u
#define STREAM_BANKS (STREAM_BASE_BANKS + STREAM_TAIL_BANKS)
#define STREAM_RING_BASE_SAMPLES (STREAM_BASE_BANKS * STREAM_BANK_LEN)
#define STREAM_RING_TAIL_SAMPLES (STREAM_TAIL_BANKS * STREAM_BANK_LEN)
#define STREAM_RING_SAMPLES (STREAM_BANKS * STREAM_BANK_LEN)

  /** One in-progress producer reservation. Samples remain invisible to the
   *  audio consumer until StreamRing_WriteCommit advances the ring writer. */
  typedef struct
  {
    uint32_t start_wr;
    uint32_t nsamp;
    uint32_t written;
    uint32_t generation;
    uint8_t voice;
    uint8_t session;
    uint8_t sof;
    uint8_t active;
  } StreamRing_Write_t;

#define STREAM_RING_WRITE_OK 0
#define STREAM_RING_WRITE_STALE 1
#define STREAM_RING_WRITE_PENDING 2
#define STREAM_RING_WRITE_FUTURE 3
#define STREAM_RING_WRITE_ERROR (-1)

  void StreamRing_Init(void);
  void StreamRing_Reset(uint8_t voice);
  void StreamRing_ResetAll(void);

  /**
   * @brief Arm consumption of the current ring session.
   */
  void StreamRing_Prime(uint8_t voice);

  /** Bind the next nX replacement to its authoritative BODY identity before
   * the command is ACKed. session 0xFF keeps legacy untagged nX behavior. */
  void StreamRing_ArmReplacement(uint8_t voice, uint16_t wave_id,
                                 uint8_t session);

  /** Retire pending/current producer ownership immediately on note-off. */
  void StreamRing_Disarm(uint8_t voice);

  /** Start a replacement session without destroying the old unread tail.
   * New BODY data begins after up to @p release_samples reserved samples. */
  uint32_t StreamRing_BeginReplacement(uint8_t voice, uint16_t wave_id,
                                       uint32_t release_samples);

  /** Read/consume the old tail reserved by StreamRing_BeginReplacement. */
  int StreamRing_GetReleaseRel(uint8_t voice, uint32_t offset, int16_t *out);
  void StreamRing_AdvanceRelease(uint8_t voice, uint32_t n);
  uint32_t StreamRing_ReleaseLevel(uint8_t voice);

  /** Stop consume and empty the FIFO (note-off). */
  void StreamRing_Release(uint8_t voice);

  /**
   * @brief Push one BODY burst into a voice ring.
   * @return nsamp accepted, or 0 if the whole burst would overflow / bad args.
   */
  uint32_t StreamRing_WriteVoice(uint8_t voice, uint8_t session, uint8_t sof,
                                 uint16_t wave_id, const int16_t *samples,
                                 uint32_t nsamp);

  /** Reserve a complete BODY burst without publishing it to the consumer.
   * @retval STREAM_RING_WRITE_OK reservation active
   * @retval STREAM_RING_WRITE_STALE prior note/session already retired
   * @retval STREAM_RING_WRITE_PENDING matching nX awaits I2S application
   * @retval STREAM_RING_WRITE_FUTURE SOF arrived before its tagged nX
   * @retval STREAM_RING_WRITE_ERROR invalid request or insufficient credit */
  int StreamRing_WriteBegin(uint8_t voice, uint8_t session, uint8_t sof,
                            uint16_t wave_id, uint32_t nsamp,
                            StreamRing_Write_t *write);

  /** Non-zero only while the note/session that owns this reservation exists. */
  uint8_t StreamRing_WriteIsCurrent(const StreamRing_Write_t *write);

  /** Contiguous writable portion of a reservation, bounded by bank/wrap. */
  int16_t *StreamRing_WriteSpan(StreamRing_Write_t *write,
                                uint32_t *nsamp_out);

  /** Record samples placed in the current writable span. */
  int StreamRing_WriteAdvance(StreamRing_Write_t *write, uint32_t nsamp);

  /** Atomically publish a completely written reservation. */
  uint32_t StreamRing_WriteCommit(StreamRing_Write_t *write);

  /** Abandon an incomplete reservation; no samples become visible. */
  void StreamRing_WriteAbort(StreamRing_Write_t *write);

  /**
   * @brief Body sample at offset from rd (0 = next unread).
   * @retval 0 ok, *out set
   * @retval -1 offset past wr (underrun / not yet written)
   */
  int StreamRing_GetRel(uint8_t voice, uint32_t offset, int16_t *out);

  /** Drop up to n unread samples from rd (playhead consumed them). */
  void StreamRing_Advance(uint8_t voice, uint32_t n);

  uint32_t StreamRing_FillLevel(uint8_t voice);

  /** Exact producer credit, including space occupied by a crash-in tail. */
  uint32_t StreamRing_FreeLevel(uint8_t voice);

  /** 1 if this session has committed at least one sample. */
  uint8_t StreamRing_HasBody(uint8_t voice);

  /** Highest fill among all voices (feedback / console). */
  uint32_t StreamRing_MaxFill(void);

  /** Lowest fill seen on a consuming voice since the last stats clear. */
  uint32_t StreamRing_MinFill(void);

  /** Record fill for min-fill (I2S consume / interpolator miss). */
  void StreamRing_ObserveFill(uint8_t voice);

  /** Whole BODY bursts dropped because the FIFO was full. */
  uint32_t StreamRing_DropCount(void);

  /** BODY bursts accepted into a ring. */
  uint32_t StreamRing_RxCount(void);

  /** Accepted armed session starts (SOF + new session). */
  uint32_t StreamRing_SofCount(void);

  /** Bursts whose samples were all zero. */
  uint32_t StreamRing_ZeroCount(void);

  void StreamRing_StatsClear(void);

  void StreamRing_DropCountClear(void);

#ifdef __cplusplus
}
#endif

#endif /* __STREAM_RING_H__ */
