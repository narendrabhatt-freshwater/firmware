/**
 ******************************************************************************
 * @file    note_filter.c
 * @brief   Per-voice 4-pole Butterworth LPF (boss four_pole_filter_t port).
 *
 * Algorithm from butterworth.cpp (init_low_pass / filter / cutoff_to_omega).
 * Coeffs, delays, and per-sample math are double (FPU). Q31 convert only at
 * Process() edges. Intentional hot-path float exception for this module.
 *
 * g = 1.0 (v1). init_high_pass kept as static for a later HP pass.
 ******************************************************************************
 */

#include "note_filter.h"

#include <math.h>
#include <stdint.h>

/* Must match I2S / CS4304 / note_bank. */
#define NOTE_FILTER_SAMPLE_RATE 96000.0

/** Damping parameter in boss formulation; 1.0 ≈ Butterworth-like. */
#define NOTE_FILTER_G 1.0

typedef struct {
  double coef[9];
  double d[4];
} NoteFilter_FourPole;

static double s_cutoff_hz[NOTE_FILTER_VOICES];
static uint8_t s_bypass[NOTE_FILTER_VOICES];
static NoteFilter_FourPole s_filt[NOTE_FILTER_VOICES];

/* ---- boss helpers (C port) ---------------------------------------------- */

static double NoteFilter_CutoffToOmega(double cutoff, double sample_rate)
{
  const double pi = 3.14159265358979323846;
  double nyquist;

  if (cutoff < 0.0) {
    cutoff = 0.0;
  }
  nyquist = 0.5 * sample_rate;
  if (cutoff > 0.999 * nyquist) {
    cutoff = 0.999 * nyquist;
  }
  return 2.0 * pi * cutoff / sample_rate;
}

/** Port of four_pole_filter_t::init_low_pass — clears delays. */
static void NoteFilter_InitLowPass(NoteFilter_FourPole *f, double omega,
                                   double g)
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

  f->d[0] = 0.0;
  f->d[1] = 0.0;
  f->d[2] = 0.0;
  f->d[3] = 0.0;
}

#if 0 /* reserved for later HP wiring — same as boss init_high_pass */
static void NoteFilter_InitHighPass(NoteFilter_FourPole *f, double omega,
                                    double g)
{
  double k, p, q, a, a0, a1, a2, a3, a4;

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

  f->d[0] = f->d[1] = f->d[2] = f->d[3] = 0.0;
}
#endif

/** Port of four_pole_filter_t::filter. */
static double NoteFilter_FilterSample(NoteFilter_FourPole *f, double in)
{
  double out = f->coef[0] * in + f->d[0];
  f->d[0] = f->coef[1] * in + f->coef[5] * out + f->d[1];
  f->d[1] = f->coef[2] * in + f->coef[6] * out + f->d[2];
  f->d[2] = f->coef[3] * in + f->coef[7] * out + f->d[3];
  f->d[3] = f->coef[4] * in + f->coef[8] * out;
  return out;
}

static int32_t NoteFilter_DoubleToQ31(double x)
{
  if (x >= 1.0) {
    return (int32_t)0x7FFFFFFF;
  }
  if (x <= -1.0) {
    return (int32_t)0x80000001;
  }
  return (int32_t)(x * 2147483647.0);
}

/* ---- public API --------------------------------------------------------- */

void NoteFilter_Reset(uint8_t voice)
{
  if (voice >= NOTE_FILTER_VOICES) {
    return;
  }
  s_filt[voice].d[0] = 0.0;
  s_filt[voice].d[1] = 0.0;
  s_filt[voice].d[2] = 0.0;
  s_filt[voice].d[3] = 0.0;
}

const char *NoteFilter_PassName(NoteFilter_Pass_t pass)
{
  if (pass == NOTE_FILTER_PASS_LP) {
    return "lp";
  }
  if (pass == NOTE_FILTER_PASS_HP) {
    return "hp";
  }
  if (pass == NOTE_FILTER_PASS_BP) {
    return "bp";
  }
  return "?";
}

NoteFilter_Pass_t NoteFilter_GetPass(uint8_t voice)
{
  (void)voice;
  return NOTE_FILTER_PASS_LP;
}

int NoteFilter_SetPass(uint8_t voice, NoteFilter_Pass_t pass)
{
  if (voice >= NOTE_FILTER_VOICES) {
    return -1;
  }
  /* v1: LPF only (boss said LPF for now). */
  if (pass != NOTE_FILTER_PASS_LP) {
    return -2;
  }
  /* Already LP; re-apply cutoff design if active. */
  if (s_cutoff_hz[voice] >= NOTE_FILTER_CUTOFF_MIN_HZ &&
      s_cutoff_hz[voice] < NOTE_FILTER_CUTOFF_MAX_HZ) {
    return NoteFilter_SetCutoff(voice, s_cutoff_hz[voice]);
  }
  return 0;
}

int NoteFilter_SetCutoff(uint8_t voice, double cutoff_hz)
{
  double omega;

  if (voice >= NOTE_FILTER_VOICES) {
    return -1;
  }
  if (cutoff_hz < NOTE_FILTER_CUTOFF_MIN_HZ ||
      cutoff_hz > NOTE_FILTER_CUTOFF_MAX_HZ) {
    return -2;
  }

  s_cutoff_hz[voice] = cutoff_hz;

  if (cutoff_hz >= NOTE_FILTER_CUTOFF_MAX_HZ) {
    s_bypass[voice] = 1u;
    NoteFilter_Reset(voice);
    return 0;
  }

  s_bypass[voice] = 0u;
  omega = NoteFilter_CutoffToOmega(cutoff_hz, NOTE_FILTER_SAMPLE_RATE);
  NoteFilter_InitLowPass(&s_filt[voice], omega, NOTE_FILTER_G);
  return 0;
}

double NoteFilter_GetCutoff(uint8_t voice)
{
  if (voice >= NOTE_FILTER_VOICES) {
    return 0.0;
  }
  if (s_cutoff_hz[voice] < NOTE_FILTER_CUTOFF_MIN_HZ) {
    return NOTE_FILTER_CUTOFF_MAX_HZ;
  }
  return s_cutoff_hz[voice];
}

int32_t NoteFilter_Process(uint8_t voice, int32_t x)
{
  double in;
  double out;

  if (voice >= NOTE_FILTER_VOICES || s_bypass[voice] != 0u) {
    return x;
  }
  if (s_cutoff_hz[voice] < NOTE_FILTER_CUTOFF_MIN_HZ) {
    return x;
  }

  in = (double)x / 2147483647.0;
  out = NoteFilter_FilterSample(&s_filt[voice], in);
  return NoteFilter_DoubleToQ31(out);
}
