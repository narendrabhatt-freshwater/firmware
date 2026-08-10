/**
 ******************************************************************************
 * @file    note_filter.c
 * @brief   Per-voice 4-pole Butterworth LPF wrapper for the note bank.
 *
 * Owns voice base/effective cutoff, pitch-track k, bypass/q tables and Q31
 * edges. DF4 math lives in butterworth_four_pole.c. Intentional hot-path
 * float exception for this module. Console q maps to DF4 g (1.0 ≈
 * Butterworth); higher → more peak near fc.
 *
 * Key follow (CMI-style): fc = fbase * (f_note/C4)^k = fbase * 2^(k n/12).
 * f0 programs fbase at C4; fk sets k; OnNoteFreq retunes on note change.
 * HP/BP deferred at the NoteFilter API.
 ******************************************************************************
 */

#include "note_filter.h"

#include "audio_rate.h"
#include "butterworth_four_pole.h"

#include <math.h>
#include <stdint.h>

/* Must match I2S / CS4304 / note_bank (audio_rate.h). */
#define NOTE_FILTER_SAMPLE_RATE ((double)AUDIO_SAMPLE_RATE_HZ)

/** Openest designed corner without entering bypass (== MAX). */
#define NOTE_FILTER_DESIGN_MAX_HZ (NOTE_FILTER_CUTOFF_MAX_HZ - 1.0)

static double s_base_hz[NOTE_FILTER_VOICES];
static double s_eff_hz[NOTE_FILTER_VOICES];
static double s_q[NOTE_FILTER_VOICES];
static double s_k[NOTE_FILTER_VOICES];
static double s_last_note_hz[NOTE_FILTER_VOICES];
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

static double NoteFilter_ClampDesignHz(double fc)
{
  if (fc < NOTE_FILTER_CUTOFF_MIN_HZ)
  {
    return NOTE_FILTER_CUTOFF_MIN_HZ;
  }
  if (fc > NOTE_FILTER_DESIGN_MAX_HZ)
  {
    return NOTE_FILTER_DESIGN_MAX_HZ;
  }
  return fc;
}

/**
 * Effective fc from base + pitch-track.
 * k==0 or unknown note → base. Track never sets the bypass flag.
 */
static double NoteFilter_ComputeEffective(uint8_t voice)
{
  double base;
  double k;
  double note;
  double ratio;
  double fc;

  base = s_base_hz[voice];
  k = s_k[voice];
  note = s_last_note_hz[voice];

  if (k <= 0.0 || !(note > 0.0))
  {
    return NoteFilter_ClampDesignHz(base);
  }

  ratio = note / NOTE_FILTER_F_REF_HZ;
  if (!(ratio > 0.0))
  {
    return NoteFilter_ClampDesignHz(base);
  }

  fc = base * pow(ratio, k);
  return NoteFilter_ClampDesignHz(fc);
}

/** Redesign LPF from effective cutoff + q. Caller: voice valid, not bypass. */
static void NoteFilter_Redesign(uint8_t voice)
{
  double omega;

  s_eff_hz[voice] = NoteFilter_ComputeEffective(voice);
  omega = ButterFourPole_CutoffToOmega(s_eff_hz[voice], NOTE_FILTER_SAMPLE_RATE);
  ButterFourPole_InitLowPass(&s_filt[voice], omega, s_q[voice]);
}

/** Re-apply design when base/k/q/note change and filter is active. */
static void NoteFilter_ApplyActive(uint8_t voice)
{
  if (s_bypass[voice] != 0u)
  {
    return;
  }
  if (s_base_hz[voice] < NOTE_FILTER_CUTOFF_MIN_HZ ||
      s_base_hz[voice] >= NOTE_FILTER_CUTOFF_MAX_HZ)
  {
    return;
  }
  NoteFilter_Redesign(voice);
}

/* ---- public API --------------------------------------------------------- */

void NoteFilter_InitAll(void)
{
  uint8_t i;

  for (i = 0u; i < NOTE_FILTER_VOICES; i++)
  {
    s_q[i] = NOTE_FILTER_Q_DEFAULT;
    s_k[i] = NOTE_FILTER_K_DEFAULT;
    s_base_hz[i] = NOTE_FILTER_CUTOFF_MAX_HZ;
    s_eff_hz[i] = NOTE_FILTER_CUTOFF_MAX_HZ;
    s_last_note_hz[i] = 0.0;
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
  /* Already LP; re-apply base design if active. */
  if (s_base_hz[voice] >= NOTE_FILTER_CUTOFF_MIN_HZ &&
      s_base_hz[voice] < NOTE_FILTER_CUTOFF_MAX_HZ)
  {
    return NoteFilter_SetCutoff(voice, s_base_hz[voice]);
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

  s_base_hz[voice] = cutoff_hz;

  if (cutoff_hz >= NOTE_FILTER_CUTOFF_MAX_HZ)
  {
    s_bypass[voice] = 1u;
    s_eff_hz[voice] = NOTE_FILTER_CUTOFF_MAX_HZ;
    NoteFilter_Reset(voice);
    return 0;
  }

  s_bypass[voice] = 0u;
  NoteFilter_Redesign(voice);
  return 0;
}

double NoteFilter_GetCutoff(uint8_t voice)
{
  if (voice >= NOTE_FILTER_VOICES)
  {
    return 0.0;
  }
  if (s_bypass[voice] != 0u)
  {
    return NOTE_FILTER_CUTOFF_MAX_HZ;
  }
  if (s_eff_hz[voice] < NOTE_FILTER_CUTOFF_MIN_HZ)
  {
    return NOTE_FILTER_CUTOFF_MAX_HZ;
  }
  return s_eff_hz[voice];
}

double NoteFilter_GetBaseCutoff(uint8_t voice)
{
  if (voice >= NOTE_FILTER_VOICES)
  {
    return 0.0;
  }
  if (s_base_hz[voice] < NOTE_FILTER_CUTOFF_MIN_HZ)
  {
    return NOTE_FILTER_CUTOFF_MAX_HZ;
  }
  return s_base_hz[voice];
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
  NoteFilter_ApplyActive(voice);
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

int NoteFilter_SetPitchK(uint8_t voice, double k)
{
  if (voice >= NOTE_FILTER_VOICES)
  {
    return -1;
  }
  if (k < NOTE_FILTER_K_MIN || k > NOTE_FILTER_K_MAX)
  {
    return -2;
  }

  s_k[voice] = k;
  NoteFilter_ApplyActive(voice);
  return 0;
}

double NoteFilter_GetPitchK(uint8_t voice)
{
  if (voice >= NOTE_FILTER_VOICES)
  {
    return NOTE_FILTER_K_DEFAULT;
  }
  if (s_k[voice] < NOTE_FILTER_K_MIN || s_k[voice] > NOTE_FILTER_K_MAX)
  {
    return NOTE_FILTER_K_DEFAULT;
  }
  return s_k[voice];
}

void NoteFilter_OnNoteFreq(uint8_t voice, double freq_hz)
{
  if (voice >= NOTE_FILTER_VOICES)
  {
    return;
  }
  if (!(freq_hz > 0.0))
  {
    return;
  }

  s_last_note_hz[voice] = freq_hz;
  /* Absolute mode (k==0): keep coeffs; only cache pitch for a later fk. */
  if (s_k[voice] > 0.0)
  {
    NoteFilter_ApplyActive(voice);
  }
}

int32_t NoteFilter_Process(uint8_t voice, int32_t x)
{
  double in;
  double out;

  if (voice >= NOTE_FILTER_VOICES || s_bypass[voice] != 0u)
  {
    return x;
  }
  if (s_eff_hz[voice] < NOTE_FILTER_CUTOFF_MIN_HZ)
  {
    return x;
  }

  in = (double)x / 2147483647.0;
  out = ButterFourPole_Process(&s_filt[voice], in);
  return NoteFilter_DoubleToQ31(out);
}
