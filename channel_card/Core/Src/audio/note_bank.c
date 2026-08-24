/**
 ******************************************************************************
 * @file    note_bank.c
 * @brief   SAMPLE voices: attack RAM + body slots → sample → LPF → env.
 *
 * Attack is an AXI head of committed length (not hold-padded). Body is
 * the USB BODY FIFO. Pitch uses 2-tap linear interpolation (Q16.16). At the join the
 * body playhead is locked to the attack source index so both sides
 * read the same sample. nX > 0 is always a note-on. Playhead changes
 * apply on the I2S sample.
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
#define INTERP_LEFT_TAPS 0u
#define BODY_ADVANCE_PHASE ((INTERP_LEFT_TAPS + 1u) * PHASE_ONE)

static double note_freq_hz[NOTE_BANK_VOICES];
static double note_scale[NOTE_BANK_VOICES];
static int32_t note_amp_q15[NOTE_BANK_VOICES];
static uint8_t note_active[NOTE_BANK_VOICES];
/* Q16.16 attack index. Body phase is relative to the ring read pointer. */
static uint64_t note_phase[NOTE_BANK_VOICES];
static uint32_t note_body_frac[NOTE_BANK_VOICES];
static int32_t note_hold[NOTE_BANK_VOICES];
static uint32_t note_inc[NOTE_BANK_VOICES];
static uint32_t note_inc_tgt[NOTE_BANK_VOICES];
/* Body consume starts at fade0. skip = source samples already past. */
static uint8_t note_body_locked[NOTE_BANK_VOICES];
static uint32_t note_body_skip[NOTE_BANK_VOICES];
static uint16_t note_wave_id[NOTE_BANK_VOICES];
static uint16_t note_play_wid[NOTE_BANK_VOICES];
static uint16_t note_play_alen[NOTE_BANK_VOICES];
static uint8_t note_body_only[NOTE_BANK_VOICES];
static NoteBank_Shape_t note_shape = NOTE_SHAPE_SINE;
static double note_shape_param = 0.5;
/* 1 = 2-tap linear; 0 = nearest sample (scope A/B). */
static uint8_t note_interp = 1u;
static volatile uint32_t note_hold_miss;

#define NOTE_CMD_NONE 0u
#define NOTE_CMD_ON 1u
#define NOTE_CMD_OFF 2u
#define NOTE_CMD_REL 3u

/* Console/USB posts; I2S drain applies. Last command wins. */
static volatile uint8_t note_cmd[NOTE_BANK_VOICES];
static volatile uint32_t note_cmd_inc[NOTE_BANK_VOICES];
static volatile float note_cmd_hz[NOTE_BANK_VOICES];
/* An explicit nX 0 must win over a late BODY session-start. */
static volatile uint8_t note_gate_requested[NOTE_BANK_VOICES];

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

static int32_t NoteBank_InterpAttack(uint16_t wid, uint64_t phase)
{
  const int16_t *tab = AttackBank_Table(wid);
  uint32_t len = AttackBank_GetLen(wid);
  uint32_t phase_q16;
  uint32_t i0;
  uint32_t i1;
  uint32_t frac;
  int32_t s0;
  int32_t s1;

  if (tab == NULL || len == 0u)
  {
    return 0;
  }
  /* Attack heads are ≤ ATTACK_BANK_LEN; Q16.16 index fits in 32 bits. */
  phase_q16 = (phase > 0xffffffffull) ? 0xffffffffu : (uint32_t)phase;
  if (note_interp == 0u)
  {
    uint32_t i = phase_q16 >> 16;
    if (i >= len)
    {
      i = len - 1u;
    }
    return ((int32_t)tab[i]) << 16;
  }
  i0 = phase_q16 >> 16;
  if (i0 >= len)
  {
    i0 = len - 1u;
  }
  i1 = (i0 + 1u < len) ? i0 + 1u : i0;
  frac = phase_q16 & 0xFFFFu;
  s0 = ((int32_t)tab[i0]) << 16;
  s1 = ((int32_t)tab[i1]) << 16;
  return (int32_t)((int64_t)s0 +
                   (((int64_t)s1 - (int64_t)s0) * frac >> 16));
}

static int NoteBank_InterpAttackBody(uint8_t note, uint16_t wid,
                                    uint64_t phase, int32_t *out)
{
  const int16_t *attack = AttackBank_Table(wid);
  uint32_t alen = AttackBank_GetLen(wid);
  int32_t source_i0 = (int32_t)(phase >> 16);
  int32_t body_i0 = (int32_t)(note_body_frac[note] >> 16);
  int32_t taps[2];
  uint32_t t;

  if (out == NULL || attack == NULL || alen == 0u)
  {
    return -1;
  }
  if (note_interp == 0u)
  {
    if (source_i0 < 0)
    {
      source_i0 = 0;
    }
    if ((uint32_t)source_i0 < alen)
    {
      *out = ((int32_t)attack[source_i0]) << 16;
      return 0;
    }
    {
      int16_t sample;
      if (body_i0 < 0 ||
          StreamRing_GetRel(note, (uint32_t)body_i0, &sample) != 0)
      {
        return -1;
      }
      *out = (int32_t)sample * 65536;
      return 0;
    }
  }
  for (t = 0u; t < 2u; t++)
  {
    int32_t source = source_i0 + (int32_t)t;
    if (source < 0)
    {
      source = 0;
    }
    if ((uint32_t)source < alen)
    {
      taps[t] = ((int32_t)attack[source]) << 16;
    }
    else
    {
      int32_t offset = body_i0 + source - source_i0;
      int16_t sample;
      if (offset < 0 ||
          StreamRing_GetRel(note, (uint32_t)offset, &sample) != 0)
      {
        return -1;
      }
      taps[t] = (int32_t)sample * 65536;
    }
  }
  *out = (int32_t)((int64_t)taps[0] +
                   (((int64_t)taps[1] - (int64_t)taps[0]) *
                    ((uint32_t)phase & 0xFFFFu) >> 16));
  return 0;
}

/**
 * Two-tap linear interpolation over the body FIFO. The next sample must
 * exist; holding the last value at a refill boundary would create a click.
 */
static int NoteBank_InterpBody(uint8_t note, int32_t *out)
{
  uint32_t phase = note_body_frac[note];
  uint32_t i0 = phase >> 16;
  uint32_t filled = StreamRing_FillLevel(note);
  int16_t s0;
  int16_t s1;

  if (out == NULL || filled == 0u || i0 >= filled)
  {
    StreamRing_ObserveFill(note);
    return -1;
  }
  if (note_interp == 0u)
  {
    int16_t sample;
    if (StreamRing_GetRel(note, i0, &sample) != 0)
    {
      StreamRing_ObserveFill(note);
      return -1;
    }
    *out = (int32_t)sample * 65536;
    return 0;
  }
  if (i0 + 1u >= filled)
  {
    StreamRing_ObserveFill(note);
    return -1;
  }

  if (StreamRing_GetRel(note, i0, &s0) != 0 ||
      StreamRing_GetRel(note, i0 + 1u, &s1) != 0)
  {
    StreamRing_ObserveFill(note);
    return -1;
  }
  *out = (int32_t)((((int64_t)s0) << 16) +
                   ((int64_t)(s1 - s0) * (phase & 0xFFFFu)));
  return 0;
}

static void NoteBank_AdvanceBody(uint8_t note)
{
  note_body_frac[note] += note_inc[note];
  while (note_body_frac[note] >= BODY_ADVANCE_PHASE)
  {
    if (StreamRing_FillLevel(note) == 0u)
    {
      break;
    }
    StreamRing_Advance(note, 1u);
    note_body_frac[note] -= PHASE_ONE;
  }
}

static void NoteBank_ClearPlayhead(uint8_t note)
{
  note_phase[note] = 0u;
  note_body_frac[note] = 0u;
  note_hold[note] = 0;
  note_body_locked[note] = 0u;
  note_body_skip[note] = 0u;
  note_body_only[note] = 0u;
}

static void NoteBank_SyncBodyPlayhead(uint8_t note, uint64_t phase,
                                     uint64_t fade0)
{
  uint64_t rel;
  uint32_t source_index;
  uint32_t history;

  if (note_body_locked[note] != 0u)
  {
    return;
  }
  rel = (phase >= fade0) ? (phase - fade0) : 0u;
  source_index = (uint32_t)(rel >> 16);
  history = (source_index < INTERP_LEFT_TAPS)
                ? source_index
                : INTERP_LEFT_TAPS;
  note_body_skip[note] = source_index - history;
  note_body_frac[note] = (history << 16) | (uint32_t)(rel & 0xFFFFu);
  note_body_locked[note] = 1u;
}

static void NoteBank_CatchUpBody(uint8_t note)
{
  uint32_t skip = note_body_skip[note];
  uint32_t filled;

  if (skip == 0u)
  {
    return;
  }
  filled = StreamRing_FillLevel(note);
  if (filled == 0u)
  {
    return;
  }
  if (skip > filled)
  {
    StreamRing_Advance(note, filled);
    note_body_skip[note] = skip - filled;
    return;
  }
  StreamRing_Advance(note, skip);
  note_body_skip[note] = 0u;
}

static void NoteBank_StartVoice(uint8_t note, uint32_t inc)
{
  StreamRing_Prime(note);
  NoteBank_ClearPlayhead(note);
  note_inc[note] = inc;
  note_inc_tgt[note] = inc;
  note_play_wid[note] = note_wave_id[note];
  note_play_alen[note] = (uint16_t)AttackBank_GetLen(note_play_wid[note]);
  note_body_only[note] = (note_play_alen[note] == 0u) ? 1u : 0u;
  NoteFilter_Reset(note);
  NoteEnv_NoteOn(note, (float)note_freq_hz[note]);
  note_active[note] = 1u;
}

static void NoteBank_PostOn(uint8_t note, uint32_t inc, float hz)
{
  note_cmd_inc[note] = inc;
  note_cmd_hz[note] = hz;
  note_cmd[note] = NOTE_CMD_ON;
}

static void NoteBank_HardOff(uint8_t note)
{
  note_freq_hz[note] = 0.0;
  note_active[note] = 0u;
  NoteBank_ClearPlayhead(note);
  StreamRing_Release(note);
  AttackBank_Stop(note);
  NoteFilter_Reset(note);
}

static void NoteBank_DrainCmd(uint8_t note)
{
  const uint8_t cmd = note_cmd[note];
  if (cmd == NOTE_CMD_NONE)
  {
    return;
  }
  note_cmd[note] = NOTE_CMD_NONE;
  if (cmd == NOTE_CMD_ON)
  {
    note_freq_hz[note] = (double)note_cmd_hz[note];
    NoteBank_StartVoice(note, note_cmd_inc[note]);
    return;
  }
  if (cmd == NOTE_CMD_REL)
  {
    note_freq_hz[note] = 0.0;
    if (NoteEnv_IsProgrammed(note) != 0u && NoteEnv_IsActive(note) != 0u)
    {
      NoteEnv_NoteOff(note);
      return;
    }
    NoteBank_HardOff(note);
    return;
  }
  if (cmd == NOTE_CMD_OFF)
  {
    NoteBank_HardOff(note);
  }
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
 * Dry sample. Head runs to its committed length. Body starts at the
 * same source index (len - overlap); the FIFO is caught up if the
 * playhead lands past that point.
 */
static int32_t NoteBank_Sample(uint8_t note)
{
  uint16_t wid = note_play_wid[note];
  uint64_t phase = note_phase[note];
  int32_t y;
  uint32_t alen = note_play_alen[note];
  uint64_t fade0;
  uint64_t fade1;

  if (note_body_only[note] != 0u)
  {
    if (NoteBank_InterpBody(note, &y) != 0)
    {
      note_hold_miss++;
      return note_hold[note];
    }
    NoteBank_AdvanceBody(note);
    note_phase[note] = phase + (uint64_t)note_inc[note];
    note_hold[note] = y;
    return y;
  }

  fade1 = (uint64_t)alen << 16;
  fade0 = (alen > SAMPLE_CROSSFADE_LEN)
              ? ((uint64_t)(alen - SAMPLE_CROSSFADE_LEN) << 16)
              : 0u;

  if (phase >= fade0)
  {
    NoteBank_SyncBodyPlayhead(note, phase, fade0);
    NoteBank_CatchUpBody(note);
  }

  if (phase < fade1)
  {
    y = NoteBank_InterpAttack(wid, phase);
    if (phase >= fade0 && note_body_skip[note] == 0u)
    {
      int32_t body;
      if (NoteBank_InterpBody(note, &body) == 0)
      {
        int32_t joined;
        if (NoteBank_InterpAttackBody(note, wid, phase, &joined) == 0)
        {
          y = joined;
        }
        NoteBank_AdvanceBody(note);
      }
    }
    note_phase[note] = phase + (uint64_t)note_inc[note];
    note_hold[note] = y;
    return y;
  }

  if (note_body_skip[note] != 0u)
  {
    return note_hold[note];
  }
  if (NoteBank_InterpBody(note, &y) != 0)
  {
    note_hold_miss++;
    return note_hold[note];
  }
  note_body_only[note] = 1u;
  NoteBank_AdvanceBody(note);
  note_phase[note] = phase + (uint64_t)note_inc[note];
  note_hold[note] = y;
  return y;
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

  if (NoteEnv_IsProgrammed(note) == 0u)
  {
    gain_q15 = (int32_t)(((int64_t)note_amp_q15[note] *
                          (int64_t)NOTE_AMP_Q15_MAX) >> 15);
    return (int32_t)(((int64_t)s * (int64_t)gain_q15) >> 15);
  }
  else
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
  note_interp = 1u;
  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    note_freq_hz[i] = 0.0;
    note_scale[i] = 0.0;
    note_amp_q15[i] = 0;
    note_active[i] = 0u;
    note_phase[i] = 0u;
    note_body_frac[i] = 0u;
    note_hold[i] = 0;
    note_inc[i] = PHASE_ONE;
    note_inc_tgt[i] = PHASE_ONE;
    note_body_locked[i] = 0u;
    note_body_skip[i] = 0u;
    note_wave_id[i] = (uint16_t)i;
    note_play_wid[i] = (uint16_t)i;
    note_play_alen[i] = 0u;
    note_body_only[i] = 0u;
    note_cmd[i] = NOTE_CMD_NONE;
    note_cmd_inc[i] = PHASE_ONE;
    note_cmd_hz[i] = 0.0f;
    note_gate_requested[i] = 0u;
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
    note_body_frac[i] = 0u;
    note_hold[i] = 0;
    note_inc[i] = PHASE_ONE;
    note_inc_tgt[i] = PHASE_ONE;
    note_body_locked[i] = 0u;
    note_body_skip[i] = 0u;
    note_play_alen[i] = 0u;
    note_body_only[i] = 0u;
    note_cmd[i] = NOTE_CMD_NONE;
    note_cmd_inc[i] = PHASE_ONE;
    note_cmd_hz[i] = 0.0f;
    note_gate_requested[i] = 0u;
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

  if (note >= NOTE_BANK_VOICES)
  {
    return;
  }

  if (freq_hz <= 0.0)
  {
    note_gate_requested[note] = 0u;
    note_freq_hz[note] = 0.0;
    note_cmd[note] = (NoteEnv_IsProgrammed(note) != 0u) ? NOTE_CMD_REL
                                                       : NOTE_CMD_OFF;
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

  inc = NoteBank_HzToInc(note_wave_id[note], freq_hz);
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
  note_gate_requested[note] = 1u;
  /* Every nX > 0 is a gate-on. Pitch slew is not this command. */
  NoteBank_PostOn(note, inc, (float)freq_hz);
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

int NoteBank_SetInterp(uint8_t enable)
{
  if (enable > 1u)
  {
    return -1;
  }
  note_interp = enable;
  return 0;
}

uint8_t NoteBank_GetInterp(void)
{
  return note_interp;
}

uint32_t NoteBank_HoldCount(void)
{
  return note_hold_miss;
}

void NoteBank_HoldCountClear(void)
{
  note_hold_miss = 0u;
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
    /* nX posts the audible start to the I2S path. Expose its requested gate
     * immediately so the first vq after the command can authorize BODY
     * during the attack instead of losing a full request round trip. This
     * does not delay or start playback; NoteBank_DrainCmd still owns that. */
    if (NoteBank_IsActive(i) == 0u && note_gate_requested[i] == 0u)
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
  uint32_t inc;

  if (voice >= NOTE_BANK_VOICES)
  {
    return;
  }
  /* New BODY session already wiped the FIFO. Do not let a delayed SOF
   * reopen a voice after its explicit note-off. */
  if (note_gate_requested[voice] == 0u)
  {
    return;
  }
  inc = note_inc_tgt[voice];
  if (inc < PHASE_INC_MIN)
  {
    inc = PHASE_INC_MIN;
  }
  if (inc > PHASE_INC_MAX)
  {
    inc = PHASE_INC_MAX;
  }
  {
    float hz = (note_freq_hz[voice] > 0.0) ? (float)note_freq_hz[voice]
                                           : note_cmd_hz[voice];
    if (!(hz > 0.0f))
    {
      hz = 1.0f;
    }
    NoteBank_PostOn(voice, inc, hz);
  }
}

int32_t NoteBank_NextSample(void)
{
  int64_t sum = 0;
  uint8_t i;
  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    NoteBank_DrainCmd(i);
    if (note_active[i] != 0u)
    {
      sum += (int64_t)NoteBank_VoiceSample(i);
    }
  }
  return NoteBank_Saturate(sum);
}
