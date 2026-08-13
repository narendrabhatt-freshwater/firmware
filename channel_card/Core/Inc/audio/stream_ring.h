/**
 ******************************************************************************
 * @file    stream_ring.h
 * @brief   Per-voice dry int16 rings filled from UAC OUT (8 ch @ 48 kHz).
 *
 * Host-pitched sustain: card consumes 1:1 (no rate-scale). Expands to Q31.
 ******************************************************************************
 */

#ifndef __STREAM_RING_H__
#define __STREAM_RING_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

#include "attack_bank.h" /* SAMPLE_VOICES */

/** Samples per voice ring (~42 ms @ 48 kHz). */
#define STREAM_RING_SAMPLES 2048u

  void StreamRing_Init(void);
  void StreamRing_Reset(uint8_t voice);
  void StreamRing_ResetAll(void);

  /**
   * @brief Note-on: empty the ring and store this note's UAC (keep oldest
   *        on overflow). Head plays from attack RAM.
   */
  void StreamRing_Prime(uint8_t voice);

  /** Stop consuming: overflow drops oldest so the ring tracks live UAC. */
  void StreamRing_Release(uint8_t voice);

  /**
   * @brief Push interleaved int16 frame (SAMPLE_VOICES channels) from UAC.
   * @param interleaved  ch0,ch1,..ch7, ch0,ch1,...
   * @param nframes      Number of multi-channel frames
   */
  void StreamRing_WriteInterleaved(const int16_t *interleaved, uint32_t nframes);

  /** Pop one dry sample as Q31; 0 on underrun. */
  int32_t StreamRing_NextSample(uint8_t voice);

  uint32_t StreamRing_FillLevel(uint8_t voice);

  /** Fill level as 0..4 quarters of STREAM_RING_SAMPLES (4 = full). */
  uint8_t StreamRing_FillQuarters(uint8_t voice);

#ifdef __cplusplus
}
#endif

#endif /* __STREAM_RING_H__ */
