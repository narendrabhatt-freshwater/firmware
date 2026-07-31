/**
 ******************************************************************************
 * @file    note_filter.c
 * @brief   Per-voice 4-pole Butterworth LPF as two cascaded SOS biquads.
 *
 * Sample rate matches the note bank / CS4304 (96 kHz). Coeffs are designed
 * with the bilinear-transform RBJ low-pass cookbook form; Q values are the
 * standard Butterworth pair for order 4.
 ******************************************************************************
 */

#include "note_filter.h"

#include <math.h>
#include <stdint.h>

/* Must match I2S / CS4304 / note_bank (see audio-dsp-conventions.mdc). */
#define NOTE_FILTER_SAMPLE_RATE 96000.0

/* Coeffs in Q28 so |a1|~2 still fits in int32 with headroom. */
#define NOTE_FILTER_Q 28
#define NOTE_FILTER_ONE (1 << NOTE_FILTER_Q)

typedef struct {
  int32_t b0;
  int32_t b1;
  int32_t b2;
  int32_t a1;
  int32_t a2;
} NoteFilter_Sos;

typedef struct {
  int32_t z1;
  int32_t z2;
} NoteFilter_State;

static double s_cutoff_hz[NOTE_FILTER_VOICES];
static uint8_t s_bypass[NOTE_FILTER_VOICES];
static NoteFilter_Sos s_sos[NOTE_FILTER_VOICES][2];
static NoteFilter_State s_st[NOTE_FILTER_VOICES][2];

/* Order-4 Butterworth section Q = 1/(2*sin((2k-1)*pi/(2*4))), k=1,2.
 * Cascade lower-Q first for better fixed-point headroom. */
static const double k_butter_q[2] = {
    0.5411961001461969, /* k=2 */
    1.3065629648763766, /* k=1 */
};

static int32_t NoteFilter_SaturateQ31(int64_t v)
{
  if (v > (int64_t)0x7FFFFFFF) {
    return (int32_t)0x7FFFFFFF;
  }
  if (v < (int64_t)(int32_t)0x80000000) {
    return (int32_t)0x80000000;
  }
  return (int32_t)v;
}

static int32_t NoteFilter_FloatToQ28(double x)
{
  double s = x * (double)NOTE_FILTER_ONE;
  if (s >= (double)0x7FFFFFFF) {
    return (int32_t)0x7FFFFFFF;
  }
  if (s <= (double)(int32_t)0x80000000) {
    return (int32_t)0x80000000;
  }
  return (int32_t)(s + (s >= 0.0 ? 0.5 : -0.5));
}

/**
 * One RBJ low-pass biquad at cutoff_hz with quality Q → Q28 SOS.
 * Cold path only (sin/cos).
 */
static void NoteFilter_DesignSos(double cutoff_hz, double q, NoteFilter_Sos *out)
{
  const double w0 = 2.0 * 3.14159265358979323846 * cutoff_hz / NOTE_FILTER_SAMPLE_RATE;
  const double cos_w0 = cos(w0);
  const double sin_w0 = sin(w0);
  const double alpha = sin_w0 / (2.0 * q);
  const double a0 = 1.0 + alpha;
  const double b0 = ((1.0 - cos_w0) * 0.5) / a0;
  const double b1 = (1.0 - cos_w0) / a0;
  const double b2 = ((1.0 - cos_w0) * 0.5) / a0;
  const double a1 = (-2.0 * cos_w0) / a0;
  const double a2 = (1.0 - alpha) / a0;

  out->b0 = NoteFilter_FloatToQ28(b0);
  out->b1 = NoteFilter_FloatToQ28(b1);
  out->b2 = NoteFilter_FloatToQ28(b2);
  out->a1 = NoteFilter_FloatToQ28(a1);
  out->a2 = NoteFilter_FloatToQ28(a2);
}

/** Transposed DF-II; coeffs Q28, sample Q31. ~tens of cycles. */
static int32_t NoteFilter_ProcessSos(const NoteFilter_Sos *c, NoteFilter_State *st,
                                     int32_t x)
{
  const int64_t y64 =
      (((int64_t)c->b0 * (int64_t)x) >> NOTE_FILTER_Q) + (int64_t)st->z1;
  const int32_t y = NoteFilter_SaturateQ31(y64);

  const int64_t z1 =
      (((int64_t)c->b1 * (int64_t)x) >> NOTE_FILTER_Q) -
      (((int64_t)c->a1 * (int64_t)y) >> NOTE_FILTER_Q) + (int64_t)st->z2;
  const int64_t z2 =
      (((int64_t)c->b2 * (int64_t)x) >> NOTE_FILTER_Q) -
      (((int64_t)c->a2 * (int64_t)y) >> NOTE_FILTER_Q);

  st->z1 = NoteFilter_SaturateQ31(z1);
  st->z2 = NoteFilter_SaturateQ31(z2);
  return y;
}

void NoteFilter_Reset(uint8_t voice)
{
  if (voice >= NOTE_FILTER_VOICES) {
    return;
  }
  s_st[voice][0].z1 = 0;
  s_st[voice][0].z2 = 0;
  s_st[voice][1].z1 = 0;
  s_st[voice][1].z2 = 0;
}

int NoteFilter_SetCutoff(uint8_t voice, double cutoff_hz)
{
  if (voice >= NOTE_FILTER_VOICES) {
    return -1;
  }
  if (cutoff_hz < NOTE_FILTER_CUTOFF_MIN_HZ ||
      cutoff_hz > NOTE_FILTER_CUTOFF_MAX_HZ) {
    return -2;
  }

  s_cutoff_hz[voice] = cutoff_hz;
  NoteFilter_Reset(voice);

  /* Max cutoff = transparent: skip MACs so default matches pre-filter bank. */
  if (cutoff_hz >= NOTE_FILTER_CUTOFF_MAX_HZ) {
    s_bypass[voice] = 1u;
    return 0;
  }

  s_bypass[voice] = 0u;
  NoteFilter_DesignSos(cutoff_hz, k_butter_q[0], &s_sos[voice][0]);
  NoteFilter_DesignSos(cutoff_hz, k_butter_q[1], &s_sos[voice][1]);
  return 0;
}

double NoteFilter_GetCutoff(uint8_t voice)
{
  if (voice >= NOTE_FILTER_VOICES) {
    return 0.0;
  }
  /* Uninitialized BSS is 0; treat as bypass max so getters match boot. */
  if (s_cutoff_hz[voice] < NOTE_FILTER_CUTOFF_MIN_HZ) {
    return NOTE_FILTER_CUTOFF_MAX_HZ;
  }
  return s_cutoff_hz[voice];
}

int32_t NoteFilter_Process(uint8_t voice, int32_t x)
{
  int32_t y;

  if (voice >= NOTE_FILTER_VOICES || s_bypass[voice] != 0u) {
    return x;
  }
  /* BSS starts bypass=0 and cutoff=0 — treat never-configured as bypass. */
  if (s_cutoff_hz[voice] < NOTE_FILTER_CUTOFF_MIN_HZ) {
    return x;
  }

  y = NoteFilter_ProcessSos(&s_sos[voice][0], &s_st[voice][0], x);
  y = NoteFilter_ProcessSos(&s_sos[voice][1], &s_st[voice][1], y);
  return y;
}
