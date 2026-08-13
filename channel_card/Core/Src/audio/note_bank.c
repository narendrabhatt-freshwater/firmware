/**
 ******************************************************************************
 * @file    note_bank.c
 * @brief   SAMPLE voices: attack RAM + body slots → sample → LPF → env.
 *
 * One Q16.16 source-index playhead per voice. Live nX slews phase_inc.
 * Join is a SAMPLE_CROSSFADE_LEN overlap mix. Body is unpitched UAC.
 ******************************************************************************
 */

#include "note_bank.h"

#include "attack_bank.h"
#include "note_envelope.h"
#include "note_filter.h"
#include "stream_ring.h"

#include <stddef.h>
#include <stdint.h>

_Static_assert(NOTE_FILTER_VOICES >= NOTE_BANK_VOICES,
               "note_filter voice count must cover note_bank");
_Static_assert(NOTE_ENV_VOICES >= NOTE_BANK_VOICES,
               "note_envelope voice count must cover note_bank");

#define NOTE_AMP_Q15_MAX 32767
#define PHASE_ONE (1u << 16)
#define PHASE_INC_MIN (PHASE_ONE / 16u)
#define PHASE_INC_MAX (PHASE_ONE * 16u)
/* ~1 octave in 20 ms @ 48 kHz. */
#define PHASE_INC_SLEW 68u

static double note_freq_hz[NOTE_BANK_VOICES];
static double note_scale[NOTE_BANK_VOICES];
static int32_t note_amp_q15[NOTE_BANK_VOICES];
static uint8_t note_active[NOTE_BANK_VOICES];
static uint16_t note_wave_id[NOTE_BANK_VOICES];
static uint32_t note_phase[NOTE_BANK_VOICES];
static uint32_t note_inc[NOTE_BANK_VOICES];
static uint32_t note_inc_tgt[NOTE_BANK_VOICES];
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

static uint32_t NoteBank_HzToInc(uint16_t wave_id, double freq_hz)
{
  float root;
  double inc;

  root = AttackBank_GetRootHz(wave_id);
  if (!(root > 0.0f) || !(freq_hz > 0.0))
  {
    return PHASE_ONE;
  }
  inc = freq_hz / (double)root;
  if (inc > 16.0)
  {
    inc = 16.0;
  }
  if (inc < (1.0 / 16.0))
  {
    inc = 1.0 / 16.0;
  }
  return (uint32_t)(inc * (double)PHASE_ONE + 0.5);
}

static int32_t NoteBank_LerpQ31(int32_t a, int32_t b, uint32_t frac)
{
  int64_t d = (int64_t)b - (int64_t)a;
  return a + (int32_t)((d * (int64_t)frac) >> 16);
}

static int32_t NoteBank_AttackAt(uint16_t wid, uint32_t idx)
{
  if (idx >= ATTACK_BANK_LEN)
  {
    return AttackBank_SampleAt(wid, ATTACK_BANK_LEN - 1u);
  }
  return AttackBank_SampleAt(wid, idx);
}

static int NoteBank_BodyAt(uint8_t note, uint32_t body_idx, int32_t *out)
{
  int16_t s16;
  if (StreamRing_Get(note, body_idx, &s16) != 0)
  {
    return -1;
  }
  *out = ((int32_t)s16) << 16;
  return 0;
}

static int32_t NoteBank_InterpAttack(uint16_t wid, uint32_t phase)
{
  uint32_t i0 = phase >> 16;
  uint32_t frac = phase & 0xFFFFu;
  int32_t a = NoteBank_AttackAt(wid, i0);
  int32_t b = NoteBank_AttackAt(wid, i0 + 1u);
  return NoteBank_LerpQ31(a, b, frac);
}

static int NoteBank_InterpBody(uint8_t note, uint32_t body_phase, int32_t *out)
{
  uint32_t i0 = body_phase >> 16;
  uint32_t frac = body_phase & 0xFFFFu;
  int32_t a;
  int32_t b;
  if (NoteBank_BodyAt(note, i0, &a) != 0)
  {
    return -1;
  }
  if (frac == 0u)
  {
    *out = a;
    return 0;
  }
  if (NoteBank_BodyAt(note, i0 + 1u, &b) != 0)
  {
    return -1;
  }
  *out = NoteBank_LerpQ31(a, b, frac);
  return 0;
}

static void NoteBank_SlewInc(uint8_t note)
{
  uint32_t inc = note_inc[note];
  uint32_t tgt = note_inc_tgt[note];
  if (inc < tgt)
  {
    uint32_t n = inc + PHASE_INC_SLEW;
    note_inc[note] = (n > tgt) ? tgt : n;
  }
  else if (inc > tgt)
  {
    uint32_t n = (inc > PHASE_INC_SLEW) ? (inc - PHASE_INC_SLEW) : 0u;
    note_inc[note] = (n < tgt) ? tgt : n;
  }
}

/**
 * Dry sample at source-index phase. 0 and no phase advance on body underrun.
 */
static int32_t NoteBank_Sample(uint8_t note)
{
  uint16_t wid = note_wave_id[note];
  uint32_t phase = note_phase[note];
  uint32_t i0 = phase >> 16;
  int32_t y;
  const uint32_t fade0 = SAMPLE_BODY_ORIGIN << 16;
  const uint32_t fade1 = ATTACK_BANK_LEN << 16;

  if (AttackBank_IsLoaded(wid) == 0u)
  {
    /* No head: body from index 0 (host still starts at BODY_ORIGIN). */
    if (NoteBank_InterpBody(note, phase, &y) != 0)
    {
      return 0;
    }
    StreamRing_DropBefore(note, i0);
    note_phase[note] = phase + note_inc[note];
    return y;
  }

  if (phase < fade0)
  {
    y = NoteBank_InterpAttack(wid, phase);
    note_phase[note] = phase + note_inc[note];
    return y;
  }

  if (phase < fade1)
  {
    int32_t atk = NoteBank_InterpAttack(wid, phase);
    uint32_t body_phase = phase - fade0;
    int32_t body;
    uint32_t t;
    uint32_t span = fade1 - fade0;
    if (NoteBank_InterpBody(note, body_phase, &body) != 0)
    {
      return 0;
    }
    t = (uint32_t)(((uint64_t)(phase - fade0) << 16) / span);
    y = NoteBank_LerpQ31(atk, body, t);
    StreamRing_DropBefore(note, body_phase >> 16);
    note_phase[note] = phase + note_inc[note];
    return y;
  }

  {
    uint32_t body_phase = phase - fade0;
    if (NoteBank_InterpBody(note, body_phase, &y) != 0)
    {
      return 0;
    }
    StreamRing_DropBefore(note, body_phase >> 16);
    note_phase[note] = phase + note_inc[note];
    return y;
  }
}

static inline int32_t NoteBank_VoiceSample(uint8_t note)
{
  int32_t s;
  float env;
  int32_t env_q15;
  int32_t gain_q15;
  int32_t amp;

  NoteBank_SlewInc(note);
  s = NoteBank_Sample(note);
  s = NoteFilter_Process(note, s);

  if (NoteEnv_IsProgrammed(note) != 0u)
  {
    env = NoteEnv_Process(note);
    if (NoteEnv_IsActive(note) == 0u)
    {
      note_active[note] = 0u;
      StreamRing_Release(note);
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
  return amp;
}

static inline int32_t NoteBank_Saturate(int64_t sum)
{
  if (sum > (int64_t)0x7FFFFFFF)
  {
    return (int32_t)0x7FFFFFFF;
  }
  if (sum < (int64_t)(int32_t)0x80000000)
  {
    return (int32_t)(int32_t)0x80000000;
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
    note_phase[i] = 0u;
    note_inc[i] = PHASE_ONE;
    note_inc_tgt[i] = PHASE_ONE;
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
    note_phase[i] = 0u;
    note_inc[i] = PHASE_ONE;
    note_inc_tgt[i] = PHASE_ONE;
    NoteFilter_Reset(i);
    if (NoteEnv_IsProgrammed(i) != 0u)
    {
      NoteEnv_NoteOff(i);
    }
  }
}

void NoteBank_SetFreq(uint8_t note, double freq_hz, double scale)
{
  uint32_t inc;
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
    StreamRing_Release(note);
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
  inc = NoteBank_HzToInc(wid, freq_hz);
  if (inc < PHASE_INC_MIN)
  {
    inc = PHASE_INC_MIN;
  }
  if (inc > PHASE_INC_MAX)
  {
    inc = PHASE_INC_MAX;
  }

  note_freq_hz[note] = freq_hz;
  note_scale[note] = scale;
  note_amp_q15[note] = NoteBank_ScaleToQ15(scale);
  note_inc_tgt[note] = inc;
  NoteFilter_OnNoteFreq(note, freq_hz);

  if (note_active[note] != 0u)
  {
    /* Live pitch: slew inc, keep source phase. Empty ring + playhead
     * already in body means underrun — treat nX as retrigger. */
    if (StreamRing_FillLevel(note) == 0u &&
        (note_phase[note] >> 16) >= SAMPLE_BODY_ORIGIN)
    {
      note_phase[note] = 0u;
      note_inc[note] = inc;
      StreamRing_Prime(note);
      NoteEnv_NoteOn(note, (float)freq_hz);
    }
    return;
  }

  StreamRing_Prime(note);
  note_phase[note] = 0u;
  note_inc[note] = inc;
  NoteEnv_NoteOn(note, (float)freq_hz);
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

void NoteBank_VoiceQuery(uint8_t *mask_out, uint8_t *best_out,
                         uint8_t *free_slots)
{
  uint8_t i;
  uint8_t mask = 0u;
  uint8_t best = 0u;
  uint32_t best_t = 0xFFFFFFFFu;
  uint8_t found = 0u;

  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    uint8_t free_s = StreamRing_FreeSlots(i);
    uint32_t filled;
    uint32_t inc;
    uint32_t t;

    if (free_slots != NULL)
    {
      free_slots[i] = free_s;
    }
    if (NoteBank_IsActive(i) == 0u)
    {
      continue;
    }
    mask = (uint8_t)(mask | (uint8_t)(1u << i));
    if (free_s == 0u)
    {
      continue;
    }
    filled = StreamRing_FillLevel(i);
    inc = note_inc[i];
    if (note_inc_tgt[i] > inc)
    {
      inc = note_inc_tgt[i];
    }
    if (inc < PHASE_INC_MIN)
    {
      inc = PHASE_INC_MIN;
    }
    t = (uint32_t)(((uint64_t)filled << 16) / (uint64_t)inc);
    if (found == 0u || t < best_t)
    {
      best_t = t;
      best = i;
      found = 1u;
    }
  }

  if (mask_out != NULL)
  {
    *mask_out = mask;
  }
  if (best_out != NULL)
  {
    *best_out = found != 0u ? best : 0xFFu;
  }
}

void NoteBank_OnBodySof(uint8_t voice)
{
  if (voice >= NOTE_BANK_VOICES)
  {
    return;
  }
  note_phase[voice] = 0u;
  note_inc[voice] = note_inc_tgt[voice];
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
