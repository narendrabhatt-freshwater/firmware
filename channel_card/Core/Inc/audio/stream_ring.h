/**
 ******************************************************************************
 * @file    stream_ring.h
 * @brief   Per-voice body rings: 8 slots × 256 int16, filled from UAC.
 *
 * UAC frames are tagged (ch0 = route, ch1..7 = samples). Card pitches via
 * a Q16.16 source-index playhead in note_bank — this ring is storage only.
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

#define STREAM_SLOTS 8u
#define STREAM_SLOT_LEN 256u
#define STREAM_RING_SAMPLES (STREAM_SLOTS * STREAM_SLOT_LEN)

/**
 * ch0 is a tag, not PCM. Host sends (TAG_BASE | voice | SOF). Near-silence
 * is idle — voice 0 must not be encoded as 0 (OS zero-pad / mute).
 */
#define STREAM_UAC_TAG_BASE 0x7F00u
#define STREAM_UAC_IDLE 0xFFu
#define STREAM_UAC_SOF 0x10u

  void StreamRing_Init(void);
  void StreamRing_Reset(uint8_t voice);
  void StreamRing_ResetAll(void);

  /**
   * @brief Arm consume. Does not clear queued samples (prefill must survive
   *        nX). Host sends SOF (route | 0x10) to start a new body.
   */
  void StreamRing_Prime(uint8_t voice);

  void StreamRing_Release(uint8_t voice);

  /**
   * @brief Push tagged UAC frames: ch0 route, ch1..7 body int16.
   */
  void StreamRing_WriteInterleaved(const int16_t *interleaved, uint32_t nframes);

  /**
   * @brief Body sample at absolute index from last SOF (0 = first prefill).
   * @retval 0 ok, *out set
   * @retval -1 missing (underrun / not yet written)
   */
  int StreamRing_Get(uint8_t voice, uint32_t body_idx, int16_t *out);

  /** Drop samples strictly before body_idx (playhead has moved on). */
  void StreamRing_DropBefore(uint8_t voice, uint32_t body_idx);

  uint32_t StreamRing_FillLevel(uint8_t voice);

  /** Complete free slots 0..8 (partial write slot does not count as free). */
  uint8_t StreamRing_FreeSlots(uint8_t voice);

#ifdef __cplusplus
}
#endif

#endif /* __STREAM_RING_H__ */
