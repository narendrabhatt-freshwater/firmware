/**
 ******************************************************************************
 * @file    note_bank.c
 * @brief   SAMPLE voices: pitched attack → UAC dry ring → env → LPF.
 *
 * Attack is rate-scaled on-card (phase_inc = note_hz / root_hz). Sustain is
 * host-pitched UAC into stream_ring, consumed 1:1.
 ******************************************************************************
 */

#include "note_bank.h"

#include "attack_bank.h"
#include "note_envelope.h"
#include "note_filter.h"
#include "stream_ring.h"

#include <math.h>
#include <stdint.h>

_Static_assert(NOTE_FILTER_VOICES >= NOTE_BANK_VOICES,
               "note_filter voice count must cover note_bank");
_Static_assert(NOTE_ENV_VOICES >= NOTE_BANK_VOICES,
               "note_envelope voice count must cover note_bank");

#define NOTE_AMP_Q15_MAX 32767

static double note_freq_hz[NOTE_BANK_VOICES];
static double note_scale[NOTE_BANK_VOICES];
static int32_t note_amp_q15[NOTE_BANK_VOICES];
static uint8_t note_active[NOTE_BANK_VOICES];
static uint16_t note_wave_id[NOTE_BANK_VOICES];
static float note_phase_inc[NOTE_BANK_VOICES];
static NoteBank_Shape_t note_shape = NOTE_SHAPE_SINE;
static double note_shape_param = 0.5;

static int32_t NoteBank_ScaleToQ15(double scale)
{
  if (scale <= 0.0)
  {
    return 0;
  }
  if (scale >= 1.0)
  {
    return NOTE_AMP_Q15_MAX;
  }
  return (int32_t)(scale * (double)NOTE_AMP_Q15_MAX + 0.5);
}

static float NoteBank_PhaseInc(uint8_t note, uint16_t wave_id, double freq_hz)
{
  float root;
  float inc;

  (void)note;
  /* Attack head only — sustain is host-pitched UAC into stream_ring. */
  root = AttackBank_GetRootHz(wave_id);
  if (!(root > 0.0f) || !(freq_hz > 0.0))
  {
    return 1.0f;
  }
  inc = (float)(freq_hz / (double)root);
  if (inc > 16.0f)
  {
    inc = 16.0f;
  }
  if (inc < (1.0f / 16.0f))
  {
    inc = 1.0f / 16.0f;
  }
  return inc;
}

/**
 * Dry sample: rate-scaled attack head, then 1:1 UAC stream ring.
 * Env/filter always applied while the voice is active.
 */
static inline int32_t NoteBank_VoiceSample(uint8_t note)
{
  int32_t s;
  float env;
  int32_t env_q15;
  int32_t gain_q15;
  int32_t amp;

  if (AttackBank_IsPlaying(note) != 0u)
  {
    s = AttackBank_NextSample(note);
  }
  else
  {
    s = StreamRing_NextSample(note);
  }

  if (NoteEnv_IsProgrammed(note) != 0u)
  {
    env = NoteEnv_Process(note);
    if (NoteEnv_IsActive(note) == 0u)
    {
      note_active[note] = 0u;
      AttackBank_Stop(note);
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

void NoteBank_Init(void)
{
  uint8_t i;

  note_shape = NOTE_SHAPE_SINE;
  note_shape_param = 0.5;
  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    note_freq_hz[i] = 0.0;
    note_scale[i] = 0.0;
    note_amp_q15[i] = 0;
    note_active[i] = 0u;
    note_wave_id[i] = i;
    note_phase_inc[i] = 1.0f;
  }
}

void NoteBank_PanicAll(void)
{
  uint8_t i;

  AttackBank_StopAll();
  StreamRing_ResetAll();
  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    note_freq_hz[i] = 0.0;
    note_scale[i] = 0.0;
    note_amp_q15[i] = 0;
    note_active[i] = 0u;
    NoteFilter_Reset(i);
    if (NoteEnv_IsProgrammed(i) != 0u)
    {
      NoteEnv_NoteOff(i);
    }
  }
}

void NoteBank_SetFreq(uint8_t note, double freq_hz, double scale)
{
  float phase_inc;
  uint16_t wid;

  if (note >= NOTE_BANK_VOICES)
  {
    return;
  }

  if (freq_hz <= 0.0)
  {
    if (NoteEnv_IsProgrammed(note) != 0u && NoteEnv_IsActive(note) != 0u)
    {
      NoteEnv_NoteOff(note);
      note_freq_hz[note] = 0.0;
      return;
    }
    note_freq_hz[note] = 0.0;
    note_active[note] = 0u;
    AttackBank_Stop(note);
    NoteFilter_Reset(note);
    return;
  }

  if (scale < 0.0)
  {
    scale = 0.0;
  }
  else if (scale > 1.0)
  {
    scale = 1.0;
  }

  wid = note_wave_id[note];
  phase_inc = NoteBank_PhaseInc(note, wid, freq_hz);

  /* Drop stale UAC dry; attack head covers fill latency when present. */
  StreamRing_Reset(note);

  note_freq_hz[note] = freq_hz;
  note_scale[note] = scale;
  note_amp_q15[note] = NoteBank_ScaleToQ15(scale);
  note_phase_inc[note] = phase_inc;
  NoteEnv_NoteOn(note, (float)freq_hz);
  NoteFilter_OnNoteFreq(note, freq_hz);

  if (AttackBank_IsLoaded(wid) != 0u)
  {
    if (AttackBank_NoteOn(note, wid, phase_inc) != 0)
    {
      note_freq_hz[note] = 0.0;
      note_active[note] = 0u;
      return;
    }
  }

  /* Sustain comes from UAC stream_ring (host-pitched); no body_bank required. */
  note_active[note] = 1u;
}

double NoteBank_GetFreq(uint8_t note)
{
  if (note >= NOTE_BANK_VOICES)
  {
    return 0.0;
  }
  return note_freq_hz[note];
}

double NoteBank_GetScale(uint8_t note)
{
  if (note >= NOTE_BANK_VOICES)
  {
    return 0.0;
  }
  return note_scale[note];
}

int NoteBank_SetWaveId(uint8_t note, uint16_t wave_id)
{
  if (note >= NOTE_BANK_VOICES || wave_id >= ATTACK_BANK_COUNT)
  {
    return -1;
  }
  note_wave_id[note] = wave_id;
  return 0;
}

uint16_t NoteBank_GetWaveId(uint8_t note)
{
  if (note >= NOTE_BANK_VOICES)
  {
    return 0u;
  }
  return note_wave_id[note];
}

int NoteBank_SetShape(NoteBank_Shape_t shape, double param)
{
  (void)param;
  if (shape != NOTE_SHAPE_SINE && shape != NOTE_SHAPE_PULSE &&
      shape != NOTE_SHAPE_TRI)
  {
    return -1;
  }
  note_shape = shape;
  note_shape_param = param;
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

uint8_t NoteBank_IsActive(uint8_t note)
{
  if (note >= NOTE_BANK_VOICES)
  {
    return 0u;
  }
  if (note_active[note] != 0u)
  {
    return 1u;
  }
  if (NoteEnv_IsProgrammed(note) != 0u && NoteEnv_IsActive(note) != 0u)
  {
    return 1u;
  }
  return 0u;
}

uint8_t NoteBank_AnyActive(void)
{
  uint8_t i;
  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    if (NoteBank_IsActive(i) != 0u)
    {
      return 1u;
    }
  }
  return 0u;
}

int32_t NoteBank_NextSample(void)
{
  int64_t sum = 0;
  uint8_t i;
  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    if (NoteBank_IsActive(i) != 0u)
    {
      sum += (int64_t)NoteBank_VoiceSample(i);
    }
  }
  return NoteBank_Saturate(sum);
}
