/**
 ******************************************************************************
 * @file    butterworth_four_pole.h
 * @brief   Reusable 4-pole DF4 Butterworth filter kernel (boss port).
 *
 * Algorithm from butterworth.cpp (init_low_pass / init_high_pass / filter /
 * cutoff_to_omega). Double coeffs + delays; callers own sample-rate and
 * fixed-point conversion.
 ******************************************************************************
 */

#ifndef __BUTTERWORTH_FOUR_POLE_H__
#define __BUTTERWORTH_FOUR_POLE_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  double coef[9];
  double d[4];
} ButterFourPole_t;

/** Clamp cutoff to [0, 0.999 * Nyquist] and return omega = 2π f / fs. */
double ButterFourPole_CutoffToOmega(double cutoff_hz, double sample_rate);

/** Design LPF at omega with g (1.0 ≈ Butterworth; higher → more peak). Clears delays. */
void ButterFourPole_InitLowPass(ButterFourPole_t *f, double omega, double g);

/** Design HPF at omega with g. Clears delays. */
void ButterFourPole_InitHighPass(ButterFourPole_t *f, double omega, double g);

/** Clear delay line only (keep coeffs). */
void ButterFourPole_Reset(ButterFourPole_t *f);

/** One sample through the DF4 section. */
double ButterFourPole_Process(ButterFourPole_t *f, double in);

#ifdef __cplusplus
}
#endif

#endif /* __BUTTERWORTH_FOUR_POLE_H__ */
