/**
 ******************************************************************************
 * @file    interp8.c
 * @brief   8-tap Hann-sinc interpolator. LUT built at init.
 ******************************************************************************
 */

#include "interp8.h"

#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int16_t s_kern[INTERP8_PHASES][INTERP8_TAPS];
static uint8_t s_ready;

static float Interp8_Sinc(float x)
{
  float pix;
  if (x > -1.0e-6f && x < 1.0e-6f)
  {
    return 1.0f;
  }
  pix = (float)M_PI * x;
  return sinf(pix) / pix;
}

void Interp8_Init(void)
{
  uint32_t p;
  uint32_t t;

  if (s_ready != 0u)
  {
    return;
  }

  for (p = 0u; p < INTERP8_PHASES; p++)
  {
    float frac = (float)p / (float)INTERP8_PHASES;
    float sum = 0.0f;
    float tap[INTERP8_TAPS];
    int32_t acc = 0;
    uint32_t last = INTERP8_TAPS - 1u;

    for (t = 0u; t < INTERP8_TAPS; t++)
    {
      /* Tap t sits at source index (i0 + t - 3); exact time is i0 + frac. */
      float x = (float)((int32_t)t - 3) - frac;
      float w = 0.0f;
      if (x > -4.0f && x < 4.0f)
      {
        w = 0.5f * (1.0f + cosf((float)M_PI * x / 4.0f));
      }
      tap[t] = Interp8_Sinc(x) * w;
      sum += tap[t];
    }
    if (sum < 1.0e-6f)
    {
      sum = 1.0f;
    }
    for (t = 0u; t < INTERP8_TAPS; t++)
    {
      int32_t q = (int32_t)lroundf((tap[t] / sum) * 32767.0f);
      if (q > 32767)
      {
        q = 32767;
      }
      if (q < -32768)
      {
        q = -32768;
      }
      s_kern[p][t] = (int16_t)q;
      acc += q;
    }
    /* Force unity DC gain after rounding. */
    s_kern[p][last] = (int16_t)((int32_t)s_kern[p][last] + (32767 - acc));
  }
  s_ready = 1u;
}

static uint32_t Interp8_Index(int32_t i, uint32_t n, uint8_t wrap)
{
  int32_t nn = (int32_t)n;
  if (n == 0u)
  {
    return 0u;
  }
  if (wrap != 0u)
  {
    i %= nn;
    if (i < 0)
    {
      i += nn;
    }
    return (uint32_t)i;
  }
  if (i < 0)
  {
    return 0u;
  }
  if (i >= nn)
  {
    return n - 1u;
  }
  return (uint32_t)i;
}

int32_t Interp8_S16(const int16_t *tab, uint32_t n, uint32_t phase_q16,
                    uint8_t wrap)
{
  uint32_t i0;
  uint32_t p;
  uint32_t t;
  int64_t acc = 0;

  if (tab == NULL || n == 0u)
  {
    return 0;
  }
  if (wrap != 0u)
  {
    uint32_t span = n << 16;
    phase_q16 %= span;
  }
  else if ((phase_q16 >> 16) >= n)
  {
    return ((int32_t)tab[n - 1u]) << 16;
  }

  i0 = phase_q16 >> 16;
  p = (phase_q16 >> 8) & (INTERP8_PHASES - 1u);
  for (t = 0u; t < INTERP8_TAPS; t++)
  {
    uint32_t idx =
        Interp8_Index((int32_t)i0 + (int32_t)t - 3, n, wrap);
    acc += (int64_t)tab[idx] * (int64_t)s_kern[p][t];
  }
  return (int32_t)((acc >> 15) << 16);
}

int32_t Interp8_Q31(const int32_t *tab, uint32_t n, uint32_t phase_q16,
                    uint8_t wrap)
{
  uint32_t i0;
  uint32_t p;
  uint32_t t;
  int64_t acc = 0;

  if (tab == NULL || n == 0u)
  {
    return 0;
  }
  if (wrap != 0u)
  {
    uint32_t span = n << 16;
    phase_q16 %= span;
  }
  else if ((phase_q16 >> 16) >= n)
  {
    return tab[n - 1u];
  }

  i0 = phase_q16 >> 16;
  p = (phase_q16 >> 8) & (INTERP8_PHASES - 1u);
  for (t = 0u; t < INTERP8_TAPS; t++)
  {
    uint32_t idx =
        Interp8_Index((int32_t)i0 + (int32_t)t - 3, n, wrap);
    acc += (int64_t)tab[idx] * (int64_t)s_kern[p][t];
  }
  return (int32_t)(acc >> 15);
}

int32_t Interp8_Q31Taps(const int32_t taps[INTERP8_TAPS],
                        uint32_t phase_q16)
{
  uint32_t p;
  uint32_t t;
  int64_t acc = 0;

  if (taps == NULL)
  {
    return 0;
  }
  p = (phase_q16 >> 8) & (INTERP8_PHASES - 1u);
  for (t = 0u; t < INTERP8_TAPS; t++)
  {
    acc += (int64_t)taps[t] * (int64_t)s_kern[p][t];
  }
  return (int32_t)(acc >> 15);
}

int32_t Interp8_S16Taps(const int16_t taps[INTERP8_TAPS], uint32_t phase_q16)
{
  uint32_t p;
  uint32_t t;
  int64_t acc = 0;

  if (taps == NULL)
  {
    return 0;
  }
  p = (phase_q16 >> 8) & (INTERP8_PHASES - 1u);
  for (t = 0u; t < INTERP8_TAPS; t++)
  {
    acc += (int64_t)taps[t] * (int64_t)s_kern[p][t];
  }
  return (int32_t)((acc >> 15) << 16);
}
