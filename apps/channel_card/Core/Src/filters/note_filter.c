/**
 ******************************************************************************
 * @file    note_filter.c
 * @brief   Per-voice 4-pole Butterworth LPF wrapper for the note bank.
 *
 * Owns voice cutoff/bypass/q tables and Q31 edges. DF4 math lives in
 * butterworth_four_pole.c. Intentional hot-path float exception for this
 * module. Console q maps to DF4 g (1.0 ≈ Butterworth); higher → more peak
 * near fc. HP/BP deferred at the NoteFilter API.
 ******************************************************************************
 */

#include "note_filter.h"

#include "audio_rate.h"
#include "butterworth_four_pole.h"

#include <stdint.h>

/* Must match I2S / CS4304 / note_bank (audio_rate.h). */
#define NOTE_FILTER_SAMPLE_RATE ((double)AUDIO_SAMPLE_RATE_HZ)

static double s_cutoff_hz[NOTE_FILTER_VOICES];
static double s_q[NOTE_FILTER_VOICES];
static uint8_t s_bypass[NOTE_FILTER_VOICES];
static ButterFourPole_t s_filt[NOTE_FILTER_VOICES];

static int32_t NoteFilter_DoubleToQ31(double x)
{
  if (x >= 1.0)
  {
    return (int32_t)0x7FFFFFFF;
  }
  if (x <= -1.0)
  {
    return (int32_t)0x80000001;
  }
  return (int32_t)(x * 2147483647.0);
}

/** Redesign LPF from stored cutoff + q. Caller guarantees voice valid + active. */
static void NoteFilter_Redesign(uint8_t voice)
{
  double omega;

  s_bypass[voice] = 0u;
  omega = ButterFourPole_CutoffToOmega(s_cutoff_hz[voice], NOTE_FILTER_SAMPLE_RATE);
  ButterFourPole_InitLowPass(&s_filt[voice], omega, s_q[voice]);
}

/* ---- public API --------------------------------------------------------- */

void NoteFilter_InitAll(void)
{
  uint8_t i;

  for (i = 0u; i < NOTE_FILTER_VOICES; i++)
  {
    s_q[i] = NOTE_FILTER_Q_DEFAULT;
    s_cutoff_hz[i] = NOTE_FILTER_CUTOFF_MAX_HZ;
    s_bypass[i] = 1u;
    ButterFourPole_Reset(&s_filt[i]);
  }
}

void NoteFilter_Reset(uint8_t voice)
{
  if (voice >= NOTE_FILTER_VOICES)
  {
    return;
  }
  ButterFourPole_Reset(&s_filt[voice]);
}

const char *NoteFilter_PassName(NoteFilter_Pass_t pass)
{
  if (pass == NOTE_FILTER_PASS_LP)
  {
    return "lp";
  }
  if (pass == NOTE_FILTER_PASS_HP)
  {
    return "hp";
  }
  if (pass == NOTE_FILTER_PASS_BP)
  {
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
  if (voice >= NOTE_FILTER_VOICES)
  {
    return -1;
  }
  /* This build: LPF only; HP/BP reserved. */
  if (pass != NOTE_FILTER_PASS_LP)
  {
    return -2;
  }
  /* Already LP; re-apply cutoff design if active. */
  if (s_cutoff_hz[voice] >= NOTE_FILTER_CUTOFF_MIN_HZ &&
      s_cutoff_hz[voice] < NOTE_FILTER_CUTOFF_MAX_HZ)
  {
    return NoteFilter_SetCutoff(voice, s_cutoff_hz[voice]);
  }
  return 0;
}

int NoteFilter_SetCutoff(uint8_t voice, double cutoff_hz)
{
  if (voice >= NOTE_FILTER_VOICES)
  {
    return -1;
  }
  /* 0 = bypass alias (same as MAX); keeps console `f 0` / `f0 0` short. */
  if (cutoff_hz == 0.0)
  {
    cutoff_hz = NOTE_FILTER_CUTOFF_MAX_HZ;
  }
  if (cutoff_hz < NOTE_FILTER_CUTOFF_MIN_HZ ||
      cutoff_hz > NOTE_FILTER_CUTOFF_MAX_HZ)
  {
    return -2;
  }

  /* q must already be valid (NoteFilter_InitAll / SetQ). Do not silent-repair. */
  if (s_q[voice] < NOTE_FILTER_Q_MIN || s_q[voice] > NOTE_FILTER_Q_MAX)
  {
    return -2;
  }

  s_cutoff_hz[voice] = cutoff_hz;

  if (cutoff_hz >= NOTE_FILTER_CUTOFF_MAX_HZ)
  {
    s_bypass[voice] = 1u;
    NoteFilter_Reset(voice);
    return 0;
  }

  NoteFilter_Redesign(voice);
  return 0;
}

double NoteFilter_GetCutoff(uint8_t voice)
{
  if (voice >= NOTE_FILTER_VOICES)
  {
    return 0.0;
  }
  if (s_cutoff_hz[voice] < NOTE_FILTER_CUTOFF_MIN_HZ)
  {
    return NOTE_FILTER_CUTOFF_MAX_HZ;
  }
  return s_cutoff_hz[voice];
}

int NoteFilter_SetQ(uint8_t voice, double q)
{
  if (voice >= NOTE_FILTER_VOICES)
  {
    return -1;
  }
  if (q < NOTE_FILTER_Q_MIN || q > NOTE_FILTER_Q_MAX)
  {
    return -2;
  }

  s_q[voice] = q;

  if (s_bypass[voice] != 0u ||
      s_cutoff_hz[voice] < NOTE_FILTER_CUTOFF_MIN_HZ ||
      s_cutoff_hz[voice] >= NOTE_FILTER_CUTOFF_MAX_HZ)
  {
    return 0;
  }

  NoteFilter_Redesign(voice);
  return 0;
}

double NoteFilter_GetQ(uint8_t voice)
{
  if (voice >= NOTE_FILTER_VOICES)
  {
    return NOTE_FILTER_Q_DEFAULT;
  }
  if (s_q[voice] < NOTE_FILTER_Q_MIN || s_q[voice] > NOTE_FILTER_Q_MAX)
  {
    return NOTE_FILTER_Q_DEFAULT;
  }
  return s_q[voice];
}

int32_t NoteFilter_Process(uint8_t voice, int32_t x)
{
  double in;
  double out;

  if (voice >= NOTE_FILTER_VOICES || s_bypass[voice] != 0u)
  {
    return x;
  }
  if (s_cutoff_hz[voice] < NOTE_FILTER_CUTOFF_MIN_HZ)
  {
    return x;
  }

  in = (double)x / 2147483647.0;
  out = ButterFourPole_Process(&s_filt[voice], in);
  return NoteFilter_DoubleToQ31(out);
}
