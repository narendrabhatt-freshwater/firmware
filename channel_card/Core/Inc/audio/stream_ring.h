/**
 ******************************************************************************
 * @file    stream_ring.h
 * @brief   Per-voice body FIFO: 3 × 4080 int16, filled from USB.
 *
 * SPSC: USB writes from main, the playhead reads in I2S. A full FIFO drops
 * the write (never unread samples). An empty FIFO is an underrun.
 *
 * BODY burst: voice + session + SOF + packed int16 samples. A new session
 * starts a new body. Repeated bursts with the same session do not reset
 * the FIFO.
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

  void StreamRing_Init(void);
  void StreamRing_Reset(uint8_t voice);
  void StreamRing_ResetAll(void);

  /**
   * @brief Arm consume. Does not clear queued samples (prefill must
   *        survive nX).
   */
  void StreamRing_Prime(uint8_t voice);

  /** Stop consume and empty the FIFO (note-off). */
  void StreamRing_Release(uint8_t voice);

  /**
   * @brief Push one BODY burst into a voice ring.
   * @return nsamp accepted, or 0 if the whole burst would overflow / bad args.
   */
  uint32_t StreamRing_WriteVoice(uint8_t voice, uint8_t session, uint8_t sof,
                                 const int16_t *samples, uint32_t nsamp);

  /**
   * @brief Body sample at offset from rd (0 = next unread).
   * @retval 0 ok, *out set
   * @retval -1 offset past wr (underrun / not yet written)
   */
  int StreamRing_GetRel(uint8_t voice, uint32_t offset, int16_t *out);

  /** Drop up to n unread samples from rd (playhead consumed them). */
  void StreamRing_Advance(uint8_t voice, uint32_t n);

  uint32_t StreamRing_FillLevel(uint8_t voice);

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

  /** Session-start (SOF + new session) resets. */
  uint32_t StreamRing_SofCount(void);

  /** Bursts whose samples were all zero. */
  uint32_t StreamRing_ZeroCount(void);

  void StreamRing_StatsClear(void);

  void StreamRing_DropCountClear(void);

#ifdef __cplusplus
}
#endif

#endif /* __STREAM_RING_H__ */
