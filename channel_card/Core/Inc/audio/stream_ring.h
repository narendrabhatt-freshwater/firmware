/**
 ******************************************************************************
 * @file    stream_ring.h
 * @brief   Per-voice BODY FIFOs stored contiguously in DTCM, filled from USB.
 *
 * Each CDC BODY block carries a voice/session and up to 1024 signed-int8
 * samples. The firmware counts processed blocks for cumulative RS485 ACKs.
 * Note-on arms the replacement origin; its first matching BODY starts it.
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

/** 85 ms of signed-int8 BODY data per voice at 48 kHz. */
#define STREAM_RING_SAMPLES 4080u

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
    uint8_t pending;
    uint8_t bank, offset;
    uint16_t head, tail, cursor;
    uint8_t active;
  } StreamRing_Write_t;

#define STREAM_RING_WRITE_OK 0
#define STREAM_RING_WRITE_STALE 1
#define STREAM_RING_WRITE_FUTURE 3
#define STREAM_RING_WRITE_ERROR (-1)

  void StreamRing_Init(void);
  void StreamRing_Reset(uint8_t voice);
  void StreamRing_ResetAll(void);

  /**
   * @brief Arm consumption of the current ring session.
   */
  void StreamRing_Prime(uint8_t voice);

  /** Create an empty pending generation. Superseding only drops old pending. */
  void StreamRing_ArmPending(uint8_t voice, uint16_t wave_id, uint8_t session);

  /** Atomically discard current remainder and promote the pending origin. */
  int StreamRing_StartNote(uint8_t voice);

  /** Reject an incoming generation without disturbing current playback. */
  void StreamRing_DiscardPending(uint8_t voice);

  /** End only the current generation; a pending generation remains armed. */
  void StreamRing_EndCurrent(uint8_t voice);

  /** Stop consume and empty the FIFO (note-off). */
  void StreamRing_Release(uint8_t voice);

  /**
   * @brief Push one BODY burst into a voice ring.
   * @return nsamp accepted, or 0 if the whole burst would overflow / bad args.
   */
  uint32_t StreamRing_WriteVoice(uint8_t voice, uint8_t session, uint8_t sof,
                                 uint16_t wave_id, const int8_t *samples,
                                 uint32_t nsamp);

  /** One complete CDC BODY block: 1 accepted, 0 retired session, -1 capacity/error. */
  int StreamRing_WriteBody(uint8_t voice, uint8_t session,
                           const int8_t *samples, uint16_t count);
  uint16_t StreamRing_LastBodySequence(void);
  void StreamRing_AudioFrame(void);
  uint8_t StreamRing_BodyAgeMs(void);

  /** Reserve a complete BODY burst without publishing it to the consumer.
   * @retval STREAM_RING_WRITE_OK reservation active
   * @retval STREAM_RING_WRITE_STALE prior note/session is no longer active
   * @retval STREAM_RING_WRITE_FUTURE SOF arrived before its tagged nX
   * @retval STREAM_RING_WRITE_ERROR invalid request or insufficient credit */
  int StreamRing_WriteBegin(uint8_t voice, uint8_t session, uint8_t sof,
                            uint16_t wave_id, uint32_t nsamp,
                            StreamRing_Write_t *write);

  /** Non-zero only while the note/session that owns this reservation exists. */
  uint8_t StreamRing_WriteIsCurrent(const StreamRing_Write_t *write);

  /** Contiguous writable portion of a reservation, bounded by bank/wrap. */
  int8_t *StreamRing_WriteSpan(StreamRing_Write_t *write,
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
  int StreamRing_GetRel(uint8_t voice, uint32_t offset, int8_t *out);
  int StreamRing_Read(uint8_t voice, uint32_t offset, int8_t *out, uint32_t count);
  uint32_t StreamRing_Consume(uint8_t voice, int8_t *out, uint32_t count);

  /** Drop up to n unread samples from rd (playhead consumed them). */
  void StreamRing_Advance(uint8_t voice, uint32_t n);

  uint32_t StreamRing_CurrentFill(uint8_t voice);
  uint32_t StreamRing_PendingFill(uint8_t voice);
  uint32_t StreamRing_FillLevel(uint8_t voice);

  uint32_t StreamRing_FreeLevel(uint8_t voice);

  uint8_t StreamRing_CurrentSession(uint8_t voice);
  uint32_t StreamRing_CurrentFree(uint8_t voice);
  uint8_t StreamRing_HasPending(uint8_t voice);
  uint8_t StreamRing_TargetSession(uint8_t voice);
  uint32_t StreamRing_TargetFill(uint8_t voice);

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
  uint32_t StreamRing_StaleCount(void);
  uint32_t StreamRing_FutureCount(void);
  uint32_t StreamRing_FullCount(void);
  uint32_t StreamRing_SupersededCount(void);

  void StreamRing_StatsClear(void);

  void StreamRing_DropCountClear(void);

#ifdef __cplusplus
}
#endif

#endif /* __STREAM_RING_H__ */
