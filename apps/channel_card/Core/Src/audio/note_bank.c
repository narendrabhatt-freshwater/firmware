/**
 ******************************************************************************
 * @file    note_bank.c
 * @brief   16-voice additive DDS bank for Channel Card CH1 (N0–NF).
 *
 * Global shape (sine LUT / runtime pulse / runtime triangle) → Q15 amp ×
 * optional multi-segment envelope → 4-pole LPF → mix.
 ******************************************************************************
 */

#include "note_bank.h"

#include "audio_rate.h"
#include "note_envelope.h"
#include "note_filter.h"

#include <stdint.h>

_Static_assert(NOTE_FILTER_VOICES == NOTE_BANK_VOICES,
               "note_filter voice count must match note_bank");
_Static_assert(NOTE_ENV_VOICES == NOTE_BANK_VOICES,
               "note_envelope voice count must match note_bank");


/* Must match I2S / CS4304 sample rate (see audio_rate.h). */
#define NOTE_BANK_SAMPLE_RATE AUDIO_SAMPLE_RATE_HZ

/*
 * Dedicated 128-entry Q31 sine table. Table size sets interpolation error
 * only; the 32-bit phase accumulator gives ~0.0000223 Hz resolution at 96 kHz
 * (finer still at 96 kHz).
 */
#define NOTE_SINE_TABLE_SIZE 128u
static const int32_t note_sine_table[NOTE_SINE_TABLE_SIZE] = {
    0x00000000,
    0x0647D97C,
    0x0C8BD35E,
    0x12C8106E,
    0x18F8B83C,
    0x1F19F97B,
    0x25280C5D,
    0x2B1F34EB,
    0x30FBC54D,
    0x36BA2013,
    0x3C56BA70,
    0x41CE1E64,
    0x471CECE6,
    0x4C3FDFF3,
    0x5133CC94,
    0x55F5A4D2,
    0x5A827999,
    0x5ED77C89,
    0x62F201AC,
    0x66CF811F,
    0x6A6D98A3,
    0x6DCA0D14,
    0x70E2CBC5,
    0x73B5EBD0,
    0x7641AF3C,
    0x78848413,
    0x7A7D055A,
    0x7C29FBED,
    0x7D8A5F3F,
    0x7E9D55FB,
    0x7F62368E,
    0x7FD8878D,
    0x7FFFFFFF,
    0x7FD8878D,
    0x7F62368E,
    0x7E9D55FB,
    0x7D8A5F3F,
    0x7C29FBED,
    0x7A7D055A,
    0x78848413,
    0x7641AF3C,
    0x73B5EBD0,
    0x70E2CBC5,
    0x6DCA0D14,
    0x6A6D98A3,
    0x66CF811F,
    0x62F201AC,
    0x5ED77C89,
    0x5A827999,
    0x55F5A4D2,
    0x5133CC94,
    0x4C3FDFF3,
    0x471CECE6,
    0x41CE1E64,
    0x3C56BA70,
    0x36BA2013,
    0x30FBC54D,
    0x2B1F34EB,
    0x25280C5D,
    0x1F19F97B,
    0x18F8B83C,
    0x12C8106E,
    0x0C8BD35E,
    0x0647D97C,
    0x00000000,
    0xF9B82684,
    0xF3742CA2,
    0xED37EF92,
    0xE70747C4,
    0xE0E60685,
    0xDAD7F3A3,
    0xD4E0CB15,
    0xCF043AB3,
    0xC945DFED,
    0xC3A94590,
    0xBE31E19C,
    0xB8E3131A,
    0xB3C0200D,
    0xAECC336C,
    0xAA0A5B2E,
    0xA57D8667,
    0xA1288377,
    0x9D0DFE54,
    0x99307EE1,
    0x9592675D,
    0x9235F2EC,
    0x8F1D343B,
    0x8C4A1430,
    0x89BE50C4,
    0x877B7BED,
    0x8582FAA6,
    0x83D60413,
    0x8275A0C1,
    0x8162AA05,
    0x809DC972,
    0x80277873,
    0x80000001,
    0x80277873,
    0x809DC972,
    0x8162AA05,
    0x8275A0C1,
    0x83D60413,
    0x8582FAA6,
    0x877B7BED,
    0x89BE50C4,
    0x8C4A1430,
    0x8F1D343B,
    0x9235F2EC,
    0x9592675D,
    0x99307EE1,
    0x9D0DFE54,
    0xA1288377,
    0xA57D8667,
    0xAA0A5B2E,
    0xAECC336C,
    0xB3C0200D,
    0xB8E3131A,
    0xBE31E19C,
    0xC3A94590,
    0xC945DFED,
    0xCF043AB3,
    0xD4E0CB15,
    0xDAD7F3A3,
    0xE0E60685,
    0xE70747C4,
    0xED37EF92,
    0xF3742CA2,
    0xF9B82684,
};

/* Cold-path Hz/scale (for getters); hot path uses phase/inc/amp_q15 only. */
static double note_freq_hz[NOTE_BANK_VOICES];
static double note_scale[NOTE_BANK_VOICES];
static uint32_t note_phase[NOTE_BANK_VOICES];
static uint32_t note_inc[NOTE_BANK_VOICES];
/* Q15 amplitude: 0 = silence, 32767 = full table peak (scale 1.0). */
static int32_t note_amp_q15[NOTE_BANK_VOICES];

#define NOTE_AMP_Q15_MAX 32767
#define NOTE_SHAPE_PEAK ((int32_t)0x7FFFFFFF)
#define NOTE_SHAPE_PARAM_MIN 0.1
#define NOTE_SHAPE_PARAM_MAX 0.9

/* Global shape — all voices share one oscillator family. */
static NoteBank_Shape_t note_shape = NOTE_SHAPE_SINE;
static double note_shape_param = 0.5;
/* Pulse: phase < thresh → +peak. Tri: rise [0, rise_end), fall after. */
static uint32_t note_pulse_thresh = 0x80000000u;
static uint32_t note_tri_rise_end = 0x80000000u;
/* floor(2^48 / seg_len); hot path uses (pos * inv) >> 16 → Q32 frac.
 * Plain floor(2^32 / len) is useless for len >= 2^31 (inv <= 2 → frac stuck at 0). */
static uint32_t note_tri_rise_inv = 131072u; /* 2^48 / 2^31 */
static uint32_t note_tri_fall_inv = 131072u;

/** Cold path only — convert Hz to 32-bit phase increment at NOTE_BANK_SAMPLE_RATE. */
static uint32_t NoteBank_PhaseIncFromHz(double freq_hz)
{
  double inc = (freq_hz * 4294967296.0) / (double)NOTE_BANK_SAMPLE_RATE;
  return (uint32_t)(inc + 0.5);
}

/** Cold path — linear scale 0.0..1.0 → Q15. */
static int32_t NoteBank_ScaleToQ15(double scale)
{
  if (scale <= 0.0) {
    return 0;
  }
  if (scale >= 1.0) {
    return NOTE_AMP_Q15_MAX;
  }
  return (int32_t)(scale * (double)NOTE_AMP_Q15_MAX + 0.5);
}

/** Cold path — param 0.1..0.9 → pulse duty / tri breakpoint + Q32 recip. */
static void NoteBank_UpdateShapeThresholds(double param)
{
  uint32_t rise_end;
  uint32_t fall_len;

  note_shape_param = param;
  rise_end = (uint32_t)(param * 4294967296.0);
  if (rise_end == 0u) {
    rise_end = 1u;
  }
  fall_len = 0u - rise_end; /* 2^32 - rise_end */
  if (fall_len == 0u) {
    fall_len = 1u;
  }

  note_pulse_thresh = rise_end;
  note_tri_rise_end = rise_end;
  note_tri_rise_inv = (uint32_t)((1ULL << 48) / (uint64_t)rise_end);
  note_tri_fall_inv = (uint32_t)((1ULL << 48) / (uint64_t)fall_len);
}

/** Map Q32 fraction 0..~1 → bipolar Q31 (-peak .. +peak). Hot path OK. */
static inline int32_t NoteBank_FracToQ31(uint32_t frac)
{
  int64_t x = ((int64_t)frac << 1) - (int64_t)4294967296LL;
  return (int32_t)((x * (int64_t)NOTE_SHAPE_PEAK) >> 32);
}

/** Q31 sample from current global shape at phase ph (no amp/LPF). */
static inline int32_t NoteBank_OscSample(uint32_t ph)
{
  switch (note_shape)
  {
  case NOTE_SHAPE_PULSE:
    return (ph < note_pulse_thresh) ? NOTE_SHAPE_PEAK : (int32_t)0x80000001;

  case NOTE_SHAPE_TRI:
    if (ph < note_tri_rise_end)
    {
      uint32_t frac =
          (uint32_t)(((uint64_t)ph * (uint64_t)note_tri_rise_inv) >> 16);
      return NoteBank_FracToQ31(frac);
    }
    {
      uint32_t pos = ph - note_tri_rise_end;
      uint32_t frac =
          (uint32_t)(((uint64_t)pos * (uint64_t)note_tri_fall_inv) >> 16);
      return -NoteBank_FracToQ31(frac);
    }

  case NOTE_SHAPE_SINE:
  default:
  {
    uint8_t idx = (uint8_t)(ph >> 25); /* 7-bit index, 0..127 */
    int32_t s0 = note_sine_table[idx];
    int32_t s1 =
        note_sine_table[(uint8_t)((idx + 1u) & (NOTE_SINE_TABLE_SIZE - 1u))];
    uint32_t frac = (ph >> 9) & 0xFFFFu;
    int64_t delta = (int64_t)s1 - (int64_t)s0;
    return s0 + (int32_t)((delta * (int64_t)frac) >> 16);
  }
  }
}

/**
 * One voice: shape osc → Q15 (scale × envelope) → LPF.
 * Programmed envelopes: keep osc alive through release; clear inc when idle.
 */
static inline int32_t NoteBank_VoiceSample(uint8_t note)
{
  uint32_t ph = note_phase[note];
  int32_t s = NoteBank_OscSample(ph);
  int32_t amp;
  float env;
  int32_t env_q15;
  int32_t gain_q15;

  note_phase[note] = ph + note_inc[note];

  if (NoteEnv_IsProgrammed(note) != 0u)
  {
    env = NoteEnv_Process(note);
    if (NoteEnv_IsActive(note) == 0u)
    {
      note_inc[note] = 0u;
      NoteFilter_Reset(note);
      return 0;
    }
  }
  else
  {
    env = 1.0f;
  }

  env_q15 = (int32_t)(env * (float)NOTE_AMP_Q15_MAX + 0.5f);
  if (env_q15 > NOTE_AMP_Q15_MAX)
  {
    env_q15 = NOTE_AMP_Q15_MAX;
  }
  if (env_q15 < 0)
  {
    env_q15 = 0;
  }
  gain_q15 =
      (int32_t)(((int64_t)note_amp_q15[note] * (int64_t)env_q15) >> 15);
  amp = (int32_t)(((int64_t)s * (int64_t)gain_q15) >> 15);
  return NoteFilter_Process(note, amp);
}

/** Saturate a 64-bit mix sum into Q31. */
static inline int32_t NoteBank_Saturate(int64_t sum)
{
  if (sum > (int64_t)0x7FFFFFFF)
  {
    return (int32_t)0x7FFFFFFF;
  }
  if (sum < (int64_t)(int32_t)0x80000000)
  {
    return (int32_t)0x80000000;
  }
  return (int32_t)sum;
}

void NoteBank_SetFreq(uint8_t note, double freq_hz, double scale)
{
  if (note >= NOTE_BANK_VOICES) {
    return;
  }

  if (freq_hz <= 0.0) {
    /* Programmed + active → release; else hard stop (legacy). */
    if (NoteEnv_IsProgrammed(note) != 0u && NoteEnv_IsActive(note) != 0u) {
      NoteEnv_NoteOff(note);
      note_freq_hz[note] = 0.0;
      /* Keep note_inc until NoteEnv_Process finishes release. */
      return;
    }
    note_freq_hz[note] = 0.0;
    note_inc[note] = 0;
    NoteFilter_Reset(note);
    return;
  }

  if (scale < 0.0) {
    scale = 0.0;
  } else if (scale > 1.0) {
    scale = 1.0;
  }

  note_freq_hz[note] = freq_hz;
  note_scale[note] = scale;
  note_inc[note] = NoteBank_PhaseIncFromHz(freq_hz);
  note_amp_q15[note] = NoteBank_ScaleToQ15(scale);
  NoteEnv_NoteOn(note, (float)freq_hz);
}

double NoteBank_GetFreq(uint8_t note)
{
  if (note >= NOTE_BANK_VOICES) {
    return 0.0;
  }
  return note_freq_hz[note];
}

double NoteBank_GetScale(uint8_t note)
{
  if (note >= NOTE_BANK_VOICES) {
    return 0.0;
  }
  return note_scale[note];
}

int NoteBank_SetShape(NoteBank_Shape_t shape, double param)
{
  if (shape == NOTE_SHAPE_SINE)
  {
    note_shape = NOTE_SHAPE_SINE;
    return 0;
  }

  if (shape != NOTE_SHAPE_PULSE && shape != NOTE_SHAPE_TRI)
  {
    return -1;
  }

  if (param < NOTE_SHAPE_PARAM_MIN || param > NOTE_SHAPE_PARAM_MAX)
  {
    return -1;
  }

  note_shape = shape;
  NoteBank_UpdateShapeThresholds(param);
  return 0;
}

NoteBank_Shape_t NoteBank_GetShape(void)
{
  return note_shape;
}

double NoteBank_GetShapeParam(void)
{
  return note_shape_param;
}

uint8_t NoteBank_AnyActive(void)
{
  for (uint8_t i = 0; i < NOTE_BANK_VOICES; i++)
  {
    if (note_inc[i] != 0u)
    {
      return 1u;
    }
  }
  return 0u;
}

int32_t NoteBank_NextSample(void)
{
  int64_t sum = 0;
  for (uint8_t i = 0; i < NOTE_BANK_VOICES; i++)
  {
    if (note_inc[i] != 0u)
    {
      sum += (int64_t)NoteBank_VoiceSample(i);
    }
  }
  return NoteBank_Saturate(sum);
}
