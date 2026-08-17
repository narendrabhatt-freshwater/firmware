/**
 ******************************************************************************
 * @file    interp8.h
 * @brief   8-tap Hann-windowed sinc (Q16.16 index → Q31).
 *
 * Linear interpolation is 2-tap. This kernel is 8-tap. Coeffs are built
 * once at init (float OK); the sample path is integer only.
 ******************************************************************************
 */

#ifndef __INTERP8_H__
#define __INTERP8_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

#define INTERP8_TAPS 8u
#define INTERP8_PHASES 256u

  void Interp8_Init(void);

  /**
   * @brief Read int16 table at Q16.16 phase.
   * @param wrap  non-zero: modulo n (sustain loop). zero: clamp ends.
   */
  int32_t Interp8_S16(const int16_t *tab, uint32_t n, uint32_t phase_q16,
                      uint8_t wrap);

  /** Same for a Q31 table (attack heads). */
  int32_t Interp8_Q31(const int32_t *tab, uint32_t n, uint32_t phase_q16,
                      uint8_t wrap);

  /**
   * @brief 8-tap kernel over prefetched int16 samples (ring body path).
   *        taps[t] is source at (i0 + t - 3); phase_q16 supplies the
   *        fractional LUT index only.
   */
  int32_t Interp8_S16Taps(const int16_t taps[INTERP8_TAPS],
                          uint32_t phase_q16);

#ifdef __cplusplus
}
#endif

#endif /* __INTERP8_H__ */
