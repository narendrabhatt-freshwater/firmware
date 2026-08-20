/**
 ******************************************************************************
 * @file    stream_ring.h
 * @brief   Per-voice body FIFO: 2 × 2048 int16 (ping-pong), filled from UAC.
 *
 * SPSC: USB writes, the playhead reads. A full FIFO drops the write
 * (never unread samples). An empty FIFO is an underrun for the reader.
 *
 * UAC frame is 10ch int16 (960 B/ms). ch0 tag: 0x7F00 | (session<<5) |
 * SOF | voice. session is 0..6. A new session starts a new body. A
 * repeated ISO frame carries the same session and does not reset the
 * FIFO. 0x7FFF is idle. ch1..9 are body for that voice.
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

/** One bank is a full jitter buffer; USB fills the other while it plays. */
#define STREAM_BANK_LEN 2048u
#define STREAM_BANKS 2u
#define STREAM_RING_SAMPLES (STREAM_BANKS * STREAM_BANK_LEN)
/** vq reports 0..14 free 256-sample slots; 15 means the ring is empty. */
#define STREAM_SLOT_LEN 256u
#define STREAM_SLOT_MAX 14u
#define STREAM_SLOT_EMPTY 15u

/**
 * ch0 is a tag, not PCM. Near-silence must not decode as voice 0
 * (OS zero-pad / mute).
 */
/** UAC interleaved width. Not the voice count (still 8). */
#define STREAM_UAC_CHANNELS 10u
#define STREAM_UAC_BODY_CH (STREAM_UAC_CHANNELS - 1u)

#define STREAM_UAC_TAG_BASE 0x7F00u
#define STREAM_UAC_IDLE 0xFFu
#define STREAM_UAC_SOF 0x10u
#define STREAM_UAC_SESSION_SHIFT 5u
#define STREAM_UAC_SESSION_MASK 0x07u
/** Session 0..6 so (session<<5)|SOF|voice cannot equal IDLE (0xFF). */
#define STREAM_UAC_SESSION_MOD 7u

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
   * @brief Push tagged UAC frames: ch0 route, ch1..9 body int16.
   */
  void StreamRing_WriteInterleaved(const int16_t *interleaved, uint32_t nframes);

  /**
   * @brief Body sample at offset from rd (0 = next unread).
   * @retval 0 ok, *out set
   * @retval -1 offset past wr (underrun / not yet written)
   */
  int StreamRing_GetRel(uint8_t voice, uint32_t offset, int16_t *out);

  /** Drop up to n unread samples from rd (playhead consumed them). */
  void StreamRing_Advance(uint8_t voice, uint32_t n);

  uint32_t StreamRing_FillLevel(uint8_t voice);

  /** Free-slot code: 0..14 complete slots; 15 means empty. */
  uint8_t StreamRing_FreeSlots(uint8_t voice);

#ifdef __cplusplus
}
#endif

#endif /* __STREAM_RING_H__ */
