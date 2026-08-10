/**
 ******************************************************************************
 * @file    butterworth_four_pole.c
 * @brief   4-pole DF4 Butterworth kernel — C port of reference four_pole_filter.
 ******************************************************************************
 */

#include "butterworth_four_pole.h"

#include <math.h>

double ButterFourPole_CutoffToOmega(double cutoff_hz, double sample_rate)
{
  const double pi = 3.14159265358979323846;
  double nyquist;

  if (cutoff_hz < 0.0)
  {
    cutoff_hz = 0.0;
  }
  nyquist = 0.5 * sample_rate;
  if (cutoff_hz > 0.999 * nyquist)
  {
    cutoff_hz = 0.999 * nyquist;
  }
  return 2.0 * pi * cutoff_hz / sample_rate;
}

void ButterFourPole_Reset(ButterFourPole_t *f)
{
  if (f == 0)
  {
    return;
  }
  f->d[0] = 0.0;
  f->d[1] = 0.0;
  f->d[2] = 0.0;
  f->d[3] = 0.0;
}

void ButterFourPole_InitLowPass(ButterFourPole_t *f, double omega, double g)
{
  double k;
  double p;
  double q;
  double a;
  double a0;
  double a1;
  double a2;
  double a3;
  double a4;

  if (f == 0)
  {
    return;
  }

  k = (4.0 * g - 3.0) / (g + 1.0);
  p = 1.0 - 0.25 * k;
  p *= p;

  a = 1.0 / (tan(0.5 * omega) * (1.0 + p));
  p = 1.0 + a;
  q = 1.0 - a;

  a0 = 1.0 / (k + p * p * p * p);
  a1 = 4.0 * (k + p * p * p * q);
  a2 = 6.0 * (k + p * p * q * q);
  a3 = 4.0 * (k + p * q * q * q);
  a4 = (k + q * q * q * q);
  p = a0 * (k + 1.0);

  f->coef[0] = p;
  f->coef[1] = 4.0 * p;
  f->coef[2] = 6.0 * p;
  f->coef[3] = 4.0 * p;
  f->coef[4] = p;
  f->coef[5] = -a1 * a0;
  f->coef[6] = -a2 * a0;
  f->coef[7] = -a3 * a0;
  f->coef[8] = -a4 * a0;

  ButterFourPole_Reset(f);
}

void ButterFourPole_InitHighPass(ButterFourPole_t *f, double omega, double g)
{
  double k;
  double p;
  double q;
  double a;
  double a0;
  double a1;
  double a2;
  double a3;
  double a4;

  if (f == 0)
  {
    return;
  }

  k = (4.0 * g - 3.0) / (g + 1.0);
  p = 1.0 - 0.25 * k;
  p *= p;

  a = tan(0.5 * omega) / (1.0 + p);
  p = a + 1.0;
  q = a - 1.0;

  a0 = 1.0 / (p * p * p * p + k);
  a1 = 4.0 * (p * p * p * q - k);
  a2 = 6.0 * (p * p * q * q + k);
  a3 = 4.0 * (p * q * q * q - k);
  a4 = (q * q * q * q + k);
  p = a0 * (k + 1.0);

  f->coef[0] = p;
  f->coef[1] = -4.0 * p;
  f->coef[2] = 6.0 * p;
  f->coef[3] = -4.0 * p;
  f->coef[4] = p;
  f->coef[5] = -a1 * a0;
  f->coef[6] = -a2 * a0;
  f->coef[7] = -a3 * a0;
  f->coef[8] = -a4 * a0;

  ButterFourPole_Reset(f);
}

double ButterFourPole_Process(ButterFourPole_t *f, double in)
{
  double out;

  if (f == 0)
  {
    return in;
  }

  out = f->coef[0] * in + f->d[0];
  f->d[0] = f->coef[1] * in + f->coef[5] * out + f->d[1];
  f->d[1] = f->coef[2] * in + f->coef[6] * out + f->d[2];
  f->d[2] = f->coef[3] * in + f->coef[7] * out + f->d[3];
  f->d[3] = f->coef[4] * in + f->coef[8] * out;
  return out;
}
