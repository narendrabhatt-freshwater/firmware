/**
 ******************************************************************************
 * @file    butterworth_four_pole.h
 * @brief   Reusable 4-pole DF4 Butterworth filter kernel.
 *
 * Direct-form algorithm ported from the reference four_pole_filter
 * implementation (init_low_pass / init_high_pass / filter /
 * cutoff_to_omega). Coefficients and delays are double; callers own
 * sample-rate selection and any fixed-point conversion at the edges.
 *
 * Parameter g (1.0 ≈ classical Butterworth) shapes peaking near fc:
 * higher g → more peak. Not RBJ biquad Q and not analog VCF resonance.
 ******************************************************************************
 */

#ifndef __BUTTERWORTH_FOUR_POLE_H__
#define __BUTTERWORTH_FOUR_POLE_H__

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Filter state: nine feedforward/feedback coefficients and four delays.
   */
  typedef struct
  {
    double coef[9];
    double d[4];
  } ButterFourPole_t;

  /**
   * @brief Convert a cutoff in Hz to digital omega = 2π·f/fs.
   * @param cutoff_hz   Desired cutoff (Hz). Clamped to [0, 0.999·Nyquist].
   * @param sample_rate Sample rate (Hz). Must match AUDIO_SAMPLE_RATE_HZ in use.
   * @return omega in radians/sample.
   */
  double ButterFourPole_CutoffToOmega(double cutoff_hz, double sample_rate);

  /**
   * @brief Design a low-pass section at omega with shape parameter g.
   * @param f     Filter instance (non-NULL).
   * @param omega Digital cutoff from ButterFourPole_CutoffToOmega().
   * @param g     Shape (typically 0.5..10; 1.0 ≈ Butterworth).
   * @note Clears delay lines after loading coefficients.
   */
  void ButterFourPole_InitLowPass(ButterFourPole_t *f, double omega, double g);

  /**
   * @brief Design a high-pass section at omega with shape parameter g.
   * @param f     Filter instance (non-NULL).
   * @param omega Digital cutoff from ButterFourPole_CutoffToOmega().
   * @param g     Shape (typically 0.5..10; 1.0 ≈ Butterworth).
   * @note Clears delay lines after loading coefficients.
   */
  void ButterFourPole_InitHighPass(ButterFourPole_t *f, double omega, double g);

  /**
   * @brief Zero delay lines only; coefficients are left unchanged.
   * @param f Filter instance (non-NULL).
   */
  void ButterFourPole_Reset(ButterFourPole_t *f);

  /**
   * @brief Process one sample through the DF4 section.
   * @param f  Filter instance (non-NULL).
   * @param in Input sample (same units as the design, typically ±1.0).
   * @return Filtered output sample.
   */
  double ButterFourPole_Process(ButterFourPole_t *f, double in);

#ifdef __cplusplus
}
#endif

#endif /* __BUTTERWORTH_FOUR_POLE_H__ */
