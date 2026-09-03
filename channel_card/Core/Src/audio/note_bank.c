/**
 ******************************************************************************
 * @file    note_bank.c
 * @brief   SAMPLE voices: attack RAM + body slots → sample → LPF → env.
 *
 * Attack is an AXI head of committed length (not hold-padded). Body is
 * the USB BODY FIFO. Pitch uses 2-tap linear interpolation (Q16.16). At the
 * join, the body playhead is locked to the attack source index so both sides
 * read the same sample. nX > 0 is always a note-on. Playhead changes apply on
 * the I2S sample.
 ******************************************************************************
 */

#include "note_bank.h"

#include "attack_bank.h"
#include "channel_vm.h"
#include "channel_led.h"
#include "note_envelope.h"
#include "note_filter.h"
#include "stream_ring.h"

#if defined(__arm__) || defined(__thumb__)
#include "main.h"
#endif

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#if defined(__arm__) || defined(__thumb__)
#include "stm32h725xx.h"
#endif

_Static_assert(NOTE_FILTER_VOICES >= NOTE_BANK_VOICES,
               "note_filter voice count must cover note_bank");
_Static_assert(NOTE_ENV_VOICE_COUNT == NOTE_BANK_VOICES,
               "note_envelope voice count must match note_bank");
_Static_assert(STREAM_BANK_LEN == 4080u,
               "Berry integration must not resize a USB BODY bank");
_Static_assert(STREAM_RING_SAMPLES == 12240u,
               "normal streaming profile must keep three BODY banks");

#define NOTE_AMP_Q15_MAX 32767
#define PHASE_ONE (1u << 16)
#define PHASE_INC_MIN (PHASE_ONE / 16u)
#define PHASE_INC_MAX (PHASE_ONE * 16u)
/* ~1 octave in 20 ms @ 48 kHz. */
#define PHASE_INC_SLEW 68u
#define INTERP_LEFT_TAPS 0u
#define BODY_ADVANCE_PHASE ((INTERP_LEFT_TAPS + 1u) * PHASE_ONE)
#define NOTE_SAMPLE_RATE_HZ 48000u
#define NOTE_DEFAULT_SCALE 0.125
#define WAVETABLE_SAMPLES 128u
#define WAVETABLE_PHASE_ONE (WAVETABLE_SAMPLES * PHASE_ONE)

static double note_freq_hz[NOTE_BANK_VOICES];
static double note_scale[NOTE_BANK_VOICES];
static int32_t note_amp_q15[NOTE_BANK_VOICES];
static int32_t note_play_amp_q15[NOTE_BANK_VOICES];
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
static uint32_t note_next_inc[NOTE_BANK_VOICES];
static float note_next_hz[NOTE_BANK_VOICES];
static int32_t note_next_amp_q15[NOTE_BANK_VOICES];
static uint16_t note_next_wid[NOTE_BANK_VOICES];
static uint8_t note_next_pending[NOTE_BANK_VOICES];
static float note_play_hz[NOTE_BANK_VOICES];
static uint8_t note_play_key[NOTE_BANK_VOICES];
static uint8_t note_play_velocity[NOTE_BANK_VOICES];
static uint8_t note_next_key[NOTE_BANK_VOICES];
static uint8_t note_next_velocity[NOTE_BANK_VOICES];
#if defined(CHANNEL_TEST_WAVETABLE)
static NoteBank_Shape_t note_shape = NOTE_SHAPE_SINE;
static int16_t note_sine_table[WAVETABLE_SAMPLES];
static volatile uint32_t note_shape_split = WAVETABLE_PHASE_ONE / 2u;
#endif
static volatile uint32_t note_hold_miss;

#define NOTE_CMD_NONE 0u
#define NOTE_CMD_ON 1u
#define NOTE_CMD_OFF 2u
#define NOTE_CMD_REL 3u

/* Console/USB posts; I2S drain applies. Last command wins. */
static volatile uint8_t note_cmd[NOTE_BANK_VOICES];
static volatile uint8_t note_cmd_key[NOTE_BANK_VOICES];
static volatile uint8_t note_cmd_velocity[NOTE_BANK_VOICES];
static volatile uint32_t note_cmd_inc[NOTE_BANK_VOICES];
static volatile float note_cmd_hz[NOTE_BANK_VOICES];
/* An explicit nX off must win over a late BODY session-start. */
static volatile uint8_t note_gate_requested[NOTE_BANK_VOICES];

static inline int32_t NoteBank_Saturate(int64_t sum);
static int NoteBank_VmDispatch(FwVmChannelHandler handler, uint8_t note);

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

#if defined(CHANNEL_TEST_WAVETABLE)
static void NoteBank_WavetableInit(void)
{
  uint32_t i;
  for (i = 0u; i < WAVETABLE_SAMPLES; i++)
  {
    double phase = (2.0 * 3.14159265358979323846 * (double)i) /
                   (double)WAVETABLE_SAMPLES;
    note_sine_table[i] = (int16_t)(sin(phase) * 32767.0);
  }
}

static uint32_t NoteBank_WavetableInc(double freq_hz)
{
  double inc = freq_hz * (double)WAVETABLE_PHASE_ONE /
               (double)NOTE_SAMPLE_RATE_HZ;
  if (inc < 1.0) inc = 1.0;
  if (inc > (double)(WAVETABLE_PHASE_ONE - 1u))
    inc = (double)(WAVETABLE_PHASE_ONE - 1u);
  return (uint32_t)(inc + 0.5);
}

static int32_t NoteBank_WavetableSample(uint32_t *phase_io, uint32_t inc)
{
  uint32_t phase = *phase_io % WAVETABLE_PHASE_ONE;
  uint32_t index = phase >> 16;
  uint32_t frac = phase & 0xFFFFu;
  int32_t y;

  if (note_shape == NOTE_SHAPE_PULSE)
  {
    y = (phase < note_shape_split) ? INT32_MAX : INT32_MIN;
  }
  else if (note_shape == NOTE_SHAPE_TRI)
  {
    uint32_t split = note_shape_split;
    if (phase < split)
    {
      y = (int32_t)(-2147483647LL +
          ((4294967294LL * (int64_t)phase) / (int64_t)split));
    }
    else
    {
      uint32_t falling = WAVETABLE_PHASE_ONE - split;
      y = (int32_t)(2147483647LL -
          ((4294967294LL * (int64_t)(phase - split)) /
           (int64_t)falling));
    }
  }
  else if (note_shape == NOTE_SHAPE_SAW)
  {
    y = (int32_t)(-2147483647LL +
        ((4294967294LL * (int64_t)phase) /
         (int64_t)WAVETABLE_PHASE_ONE));
  }
  else
  {
    int32_t s0 = note_sine_table[index];
    int32_t s1 = note_sine_table[(index + 1u) & (WAVETABLE_SAMPLES - 1u)];
    y = (int32_t)(((int64_t)s0 << 16) +
                  ((int64_t)(s1 - s0) * (int64_t)frac));
  }
  phase += inc;
  if (phase >= WAVETABLE_PHASE_ONE) phase %= WAVETABLE_PHASE_ONE;
  *phase_io = phase;
  return y;
}
#endif

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
 * Two-tap linear interpolation over the body FIFO. Count every miss at the
 * actual BODY boundary, including a late first frame. Continuing with the
 * last valid sample hides a broken USB stream and produces plausible but
 * incorrect audio, so production firmware fails closed here.
 */
static int32_t NoteBank_BodyMiss(uint8_t note)
{
  note_hold_miss++;
#if defined(__arm__) || defined(__thumb__)
  Error_Handler();
#endif
  return note_hold[note];
}

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

static void NoteBank_ActivateReplacement(uint8_t note)
{
  if (note_next_pending[note] == 0u)
  {
    return;
  }
  NoteBank_ClearPlayhead(note);
  note_inc[note] = note_next_inc[note];
  note_inc_tgt[note] = note_next_inc[note];
  note_play_amp_q15[note] = note_next_amp_q15[note];
  note_play_hz[note] = note_next_hz[note];
  note_play_key[note] = note_next_key[note];
  note_play_velocity[note] = note_next_velocity[note];
  note_play_wid[note] = note_next_wid[note];
  note_play_alen[note] = (uint16_t)AttackBank_GetLen(note_play_wid[note]);
  note_body_only[note] = (note_play_alen[note] == 0u) ? 1u : 0u;
  NoteFilter_Reset(note);
  NoteFilter_OnNoteFreq(note, (double)note_next_hz[note]);
  note_next_pending[note] = 0u;
  note_next_key[note] = 0u;
  note_next_velocity[note] = 0u;
  note_active[note] = 1u;
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

static void NoteBank_StartVoice(uint8_t note, uint8_t key, uint8_t velocity,
                                uint32_t inc, float hz)
{
  note_next_inc[note] = inc;
  note_next_hz[note] = hz;
  note_next_key[note] = key;
  note_next_velocity[note] = velocity;
  note_next_amp_q15[note] = note_amp_q15[note];
  note_next_wid[note] = note_wave_id[note];
  note_next_pending[note] = 1u;

}

static void NoteBank_PostOn(uint8_t note, uint8_t key, uint8_t velocity,
                            uint32_t inc, float hz)
{
  note_cmd_key[note] = key;
  note_cmd_velocity[note] = velocity;
  note_cmd_inc[note] = inc;
  note_cmd_hz[note] = hz;
  note_cmd[note] = NOTE_CMD_ON;
}

static void NoteBank_HardOff(uint8_t note)
{
  note_freq_hz[note] = 0.0;
  note_active[note] = 0u;
  note_next_pending[note] = 0u;
  note_play_key[note] = 0u;
  note_play_velocity[note] = 0u;
  note_next_key[note] = 0u;
  note_next_velocity[note] = 0u;
  NoteBank_ClearPlayhead(note);
  StreamRing_Release(note);
  AttackBank_Stop(note);
  NoteFilter_Reset(note);
  NoteEnv_Stop(note);
}

static void NoteBank_EndCurrent(uint8_t note)
{
  note_freq_hz[note] = 0.0;
  note_active[note] = 0u;
  note_play_key[note] = 0u;
  note_play_velocity[note] = 0u;
  NoteBank_ClearPlayhead(note);
  StreamRing_EndCurrent(note);
  AttackBank_Stop(note);
  NoteFilter_Reset(note);
  NoteEnv_Stop(note);
}

static void NoteBank_DrainCmd(uint8_t note)
{
  const uint8_t cmd = note_cmd[note];
  if (cmd == NOTE_CMD_NONE)
  {
    return;
  }
  if (cmd == NOTE_CMD_ON)
  {
    if (ChannelVm_IsActive(note) == 0u)
    {
      note_cmd[note] = NOTE_CMD_NONE;
      NoteBank_HardOff(note);
      return;
    }
#if !defined(CHANNEL_TEST_WAVETABLE)
    /* Authority alone cannot change musical state. Wait for one whole BODY
     * frame, retaining the current note indefinitely if transport stalls. */
    if (StreamRing_HasBody(note) == 0u) return;
#endif
    note_cmd[note] = NOTE_CMD_NONE;
    NoteBank_StartVoice(note, note_cmd_key[note], note_cmd_velocity[note],
                        note_cmd_inc[note],
                        note_cmd_hz[note]);
    (void)NoteBank_VmDispatch(FW_VM_CHANNEL_HANDLER_NOTE_ON, note);
    return;
  }
  note_cmd[note] = NOTE_CMD_NONE;
  if (cmd == NOTE_CMD_REL)
  {
    /* Note-off always cancels a not-yet-started replacement. Pending BODY is
     * transport state, so envelope scripts do not need to manage it. */
    StreamRing_DiscardPending(note);
    note_next_pending[note] = 0u;
    note_next_key[note] = 0u;
    note_next_velocity[note] = 0u;
    if (ChannelVm_IsActive(note) == 0u) NoteBank_HardOff(note);
    else (void)NoteBank_VmDispatch(FW_VM_CHANNEL_HANDLER_NOTE_OFF, note);
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
#if defined(CHANNEL_TEST_WAVETABLE)
  {
    uint32_t phase = (uint32_t)note_phase[note];
    int32_t sample = NoteBank_WavetableSample(&phase, note_inc[note]);
    note_phase[note] = phase;
    note_hold[note] = sample;
    return sample;
  }
#endif
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
      return NoteBank_BodyMiss(note);
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
      else
      {
        /* The local attack is still authoritative. BODY may arrive any time
         * before the attack exhausts; account for a miss only at the actual
         * BODY boundary. */
      }
    }
    note_phase[note] = phase + (uint64_t)note_inc[note];
    note_hold[note] = y;
    return y;
  }

  if (note_body_skip[note] != 0u)
  {
    return NoteBank_BodyMiss(note);
  }
  if (NoteBank_InterpBody(note, &y) != 0)
  {
    return NoteBank_BodyMiss(note);
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

  env = NoteEnv_RenderSample(note);
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
      (int32_t)(((int64_t)note_play_amp_q15[note] * (int64_t)env_q15) >> 15);
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

static int NoteBank_VmRead(void *context, uint8_t note,
                           FwVmChannelInput value, float *out)
{
  (void)context;
  if (out == NULL || note >= NOTE_BANK_VOICES) return -1;
  switch (value)
  {
  case FW_VM_CHANNEL_INPUT_NOTE_ID: *out = (float)note; break;
  case FW_VM_CHANNEL_INPUT_FREQUENCY:
    *out = (note_active[note] != 0u) ? note_play_hz[note] : note_next_hz[note]; break;
  case FW_VM_CHANNEL_INPUT_GAIN:
    *out = (float)((note_active[note] != 0u) ? note_play_amp_q15[note]
                                             : note_next_amp_q15[note]) /
           (float)NOTE_AMP_Q15_MAX; break;
  case FW_VM_CHANNEL_INPUT_GATE: *out = note_gate_requested[note] != 0u ? 1.0f : 0.0f; break;
  case FW_VM_CHANNEL_INPUT_ACTIVE: *out = note_active[note] != 0u ? 1.0f : 0.0f; break;
  case FW_VM_CHANNEL_INPUT_HAS_PENDING: *out = note_next_pending[note] != 0u ? 1.0f : 0.0f; break;
  case FW_VM_CHANNEL_INPUT_PENDING_FREQUENCY: *out = note_next_hz[note]; break;
  case FW_VM_CHANNEL_INPUT_PENDING_GAIN:
    *out = (float)note_next_amp_q15[note] / (float)NOTE_AMP_Q15_MAX; break;
  case FW_VM_CHANNEL_INPUT_AMPLITUDE: *out = NoteEnv_Amplitude(note); break;
  case FW_VM_CHANNEL_INPUT_KEY: *out = (float)note_play_key[note]; break;
  case FW_VM_CHANNEL_INPUT_PENDING_KEY: *out = (float)note_next_key[note]; break;
  case FW_VM_CHANNEL_INPUT_VELOCITY: *out = (float)note_play_velocity[note]; break;
  case FW_VM_CHANNEL_INPUT_PENDING_VELOCITY: *out = (float)note_next_velocity[note]; break;
  default: return -1;
  }
  return 0;
}

static int NoteBank_VmSet(void *context, uint8_t note, float amplitude)
{ (void)context; return NoteEnv_SetAmplitude(note, amplitude); }
static int NoteBank_VmRamp(void *context, uint8_t note, float target, float slope)
{
  (void)context;
  if (!isfinite(slope) || !(slope / (float)NOTE_SAMPLE_RATE_HZ > 0.0f))
    return -1;
  return NoteEnv_StartRamp(note, target, slope);
}
static int NoteBank_VmHold(void *context, uint8_t note)
{ (void)context; return NoteEnv_Hold(note); }
static int NoteBank_VmStartNoteAt(void *context, uint8_t note,
                                  float frequency_hz)
{
  uint32_t inc;
  (void)context;
  if (note >= NOTE_BANK_VOICES || note_next_pending[note] == 0u ||
      !isfinite(frequency_hz) || !(frequency_hz > 0.0f))
  {
    return -1;
  }
  inc = NoteBank_HzToInc(note_next_wid[note], (double)frequency_hz);
  if (inc < PHASE_INC_MIN) inc = PHASE_INC_MIN;
  if (inc > PHASE_INC_MAX) inc = PHASE_INC_MAX;
  if (StreamRing_StartNote(note) != 0) return -1;
  note_next_hz[note] = frequency_hz;
  note_next_inc[note] = inc;
  NoteBank_ActivateReplacement(note);
  note_freq_hz[note] = note_play_hz[note];
  return 0;
}
static int NoteBank_VmStartNote(void *context, uint8_t note)
{
  (void)context;
  if (note_next_pending[note] == 0u) return -1;
  if (StreamRing_StartNote(note) != 0) return -1;
  NoteBank_ActivateReplacement(note);
  note_freq_hz[note] = note_play_hz[note];
  return 0;
}
static int NoteBank_VmEnd(void *context, uint8_t note)
{ (void)context; NoteBank_EndCurrent(note); return 0; }
static int NoteBank_VmDiscardPending(void *context, uint8_t note)
{
  (void)context;
  if (note >= NOTE_BANK_VOICES) return -1;
  StreamRing_DiscardPending(note);
  note_next_pending[note] = 0u;
  note_next_key[note] = 0u;
  note_next_velocity[note] = 0u;
  return 0;
}
static void NoteBank_VmSilence(void *context, uint8_t note, FwVmFault fault)
{ (void)context; (void)fault; NoteBank_HardOff(note); }
static int NoteBank_VmLed(void *context, uint8_t note, float red, float green,
                          float blue, float brightness)
{
  (void)context; (void)note;
  return ChannelLed_Set(red, green, blue, brightness);
}

static int NoteBank_VmDispatch(FwVmChannelHandler handler, uint8_t note)
{
#if defined(__arm__) || defined(__thumb__)
  uint32_t start = DWT->CYCCNT;
  int result = ChannelVm_Dispatch(handler, note);
  ChannelVm_RecordCycles(note, DWT->CYCCNT - start);
  return result;
#else
  return ChannelVm_Dispatch(handler, note);
#endif
}

void NoteBank_Init(void)
{
  uint8_t i;
  ChannelVmNativeOps vm_ops = {0};

#if defined(CHANNEL_TEST_WAVETABLE)
  note_shape = NOTE_SHAPE_SINE;
  note_shape_split = WAVETABLE_PHASE_ONE / 2u;
  NoteBank_WavetableInit();
#endif
  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    note_freq_hz[i] = 0.0;
    note_scale[i] = 0.0;
    note_amp_q15[i] = 0;
    note_play_amp_q15[i] = 0;
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
    note_next_inc[i] = PHASE_ONE;
    note_next_hz[i] = 0.0f;
    note_next_amp_q15[i] = 0;
    note_next_wid[i] = (uint16_t)i;
    note_next_pending[i] = 0u;
    note_play_hz[i] = 0.0f;
    note_play_key[i] = 0u;
    note_play_velocity[i] = 0u;
    note_next_key[i] = 0u;
    note_next_velocity[i] = 0u;
    note_cmd[i] = NOTE_CMD_NONE;
    note_cmd_inc[i] = PHASE_ONE;
    note_cmd_hz[i] = 0.0f;
    note_cmd_velocity[i] = 0u;
    note_gate_requested[i] = 0u;
  }
  vm_ops.context = NULL;
  vm_ops.read_input = NoteBank_VmRead;
  vm_ops.set_amplitude = NoteBank_VmSet;
  vm_ops.ramp = NoteBank_VmRamp;
  vm_ops.hold = NoteBank_VmHold;
  vm_ops.start_note = NoteBank_VmStartNote;
  vm_ops.note_end = NoteBank_VmEnd;
  vm_ops.silence_voice = NoteBank_VmSilence;
  vm_ops.set_led = NoteBank_VmLed;
  vm_ops.discard_pending = NoteBank_VmDiscardPending;
  vm_ops.start_note_at = NoteBank_VmStartNoteAt;
#if defined(__arm__) || defined(__thumb__)
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0u;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
#endif
  ChannelVm_Init(&vm_ops);
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
    note_play_amp_q15[i] = 0;
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
    note_next_pending[i] = 0u;
    note_cmd[i] = NOTE_CMD_NONE;
    note_cmd_inc[i] = PHASE_ONE;
    note_cmd_hz[i] = 0.0f;
    note_gate_requested[i] = 0u;
    note_play_key[i] = 0u;
    note_play_velocity[i] = 0u;
    note_next_key[i] = 0u;
    note_next_velocity[i] = 0u;
    note_cmd_velocity[i] = 0u;
    NoteFilter_Reset(i);
    NoteEnv_Stop(i);
  }
}

static int NoteBank_NoteOnBound(uint8_t note, uint8_t key, uint8_t velocity,
                                uint8_t session)
{
  double freq_hz;
  double scale = NOTE_DEFAULT_SCALE;
  uint32_t inc;

  if (note >= NOTE_BANK_VOICES || key >= FW_SCRIPT_CHANNEL_KEY_COUNT ||
      velocity == 0u || velocity > 127u)
  {
    return -1;
  }
  if (ChannelVm_UploadIsBusy() != 0u)
  {
    return -3;
  }
  if (ChannelVm_IsActive(note) == 0u)
  {
    return -2;
  }

  freq_hz = (double)fw_vm_channel_standard_hz(key);
  if (!(freq_hz > 0.0) || !isfinite(freq_hz)) return -1;

  inc = NoteBank_HzToInc(note_wave_id[note], freq_hz);
#if defined(CHANNEL_TEST_WAVETABLE)
  inc = NoteBank_WavetableInc(freq_hz);
#endif
#if !defined(CHANNEL_TEST_WAVETABLE)
  if (inc < PHASE_INC_MIN)
  {
    inc = PHASE_INC_MIN;
  }
  if (inc > PHASE_INC_MAX)
  {
    inc = PHASE_INC_MAX;
  }
#endif

  note_freq_hz[note] = freq_hz;
  note_scale[note] = scale;
  note_amp_q15[note] = NoteBank_ScaleToQ15(scale);
  note_inc_tgt[note] = inc;
  note_gate_requested[note] = 1u;
  StreamRing_ArmPending(note, note_wave_id[note], session);
  NoteBank_PostOn(note, key, velocity, inc, (float)freq_hz);
  return 0;
}

int NoteBank_NoteOn(uint8_t note, uint8_t key, uint8_t velocity)
{
  return NoteBank_NoteOnBound(note, key, velocity, 0xFFu);
}

int NoteBank_NoteOnSession(uint8_t note, uint8_t key, uint8_t velocity,
                           uint8_t session)
{
  if (session == 0xFFu)
  {
    return -1;
  }
  return NoteBank_NoteOnBound(note, key, velocity, session);
}

int NoteBank_NoteOff(uint8_t note)
{
  if (note >= NOTE_BANK_VOICES) return -1;
  note_gate_requested[note] = 0u;
  note_freq_hz[note] = 0.0;
  note_cmd[note] = NOTE_CMD_REL;
  return 0;
}

uint8_t NoteBank_GetKey(uint8_t note)
{
  if (note >= NOTE_BANK_VOICES) return 0u;
  return note_next_pending[note] != 0u ? note_next_key[note] : note_play_key[note];
}

uint8_t NoteBank_GetVelocity(uint8_t note)
{
  if (note >= NOTE_BANK_VOICES) return 0u;
  return note_next_pending[note] != 0u ? note_next_velocity[note]
                                       : note_play_velocity[note];
}

double NoteBank_GetFreq(uint8_t note)
{
  return note < NOTE_BANK_VOICES ? note_freq_hz[note] : 0.0;
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

#if defined(CHANNEL_TEST_WAVETABLE)
int NoteBank_SetShape(NoteBank_Shape_t shape, double param)
{
  if (shape != NOTE_SHAPE_SINE && shape != NOTE_SHAPE_PULSE &&
      shape != NOTE_SHAPE_TRI && shape != NOTE_SHAPE_SAW)
  {
    return -1;
  }
  note_shape = shape;
  if (shape == NOTE_SHAPE_PULSE || shape == NOTE_SHAPE_TRI)
  {
    double limited = param;
    if (limited < 0.1) limited = 0.1;
    if (limited > 0.9) limited = 0.9;
    note_shape_split = (uint32_t)(limited * (double)WAVETABLE_PHASE_ONE);
  }
  return 0;
}
#endif

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

void NoteBank_VoiceQuery(uint8_t *mask_out, uint8_t *best_out)
{
  uint8_t i;
  uint8_t mask = 0u;
  uint8_t best = 0u;
  uint32_t best_t = 0xFFFFFFFFu;
  uint8_t found = 0u;

  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    uint32_t filled;
    uint32_t inc;
    uint32_t t;

    if (NoteBank_IsActive(i) != 0u)
    {
      mask = (uint8_t)(mask | (uint8_t)(1u << i));
    }
    if (NoteBank_IsActive(i) == 0u && StreamRing_HasPending(i) == 0u)
    {
      continue;
    }
#if defined(CHANNEL_TEST_WAVETABLE)
    continue;
#endif
    filled = StreamRing_TargetFill(i);
    if (filled >= STREAM_RING_SAMPLES)
    {
      continue;
    }
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


int32_t NoteBank_NextSample(void)
{
  int64_t sum = 0;
  uint8_t i;
  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    if (note_active[i] != 0u)
    {
      sum += (int64_t)NoteBank_VoiceSample(i);
    }
  }
  return NoteBank_Saturate(sum);
}

void NoteBank_VmBoundaryBegin(void)
{
  uint8_t i;
  ChannelVm_BoundaryBegin();
  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    NoteBank_DrainCmd(i);
  }
}

void NoteBank_VmBoundaryEnd(void)
{
  uint8_t i;
  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    if (NoteEnv_TakeRampEnd(i) != 0u)
    {
      (void)NoteBank_VmDispatch(FW_VM_CHANNEL_HANDLER_RAMP_END, i);
    }
  }
}

void NoteBank_VmStop(uint8_t voice)
{
  ChannelVm_Stop(voice);
}

void NoteBank_VmStopAll(void)
{
  ChannelVm_StopAll();
}

uint8_t NoteBank_VmIsActive(uint8_t voice)
{
  return ChannelVm_IsActive(voice);
}

uint8_t NoteBank_VmActiveMask(void)
{
  return ChannelVm_ActiveMask();
}

int NoteBank_VmUploadBegin(uint8_t voice)
{
  if (NoteBank_AnyActive() != 0u) return -2;
  return ChannelVm_UploadBegin(voice);
}

int NoteBank_VmUploadFeed(uint8_t voice, const void *data, size_t size)
{
  return ChannelVm_UploadFeed(voice, data, size);
}

int NoteBank_VmUploadCommit(uint8_t voice)
{
  int result = ChannelVm_UploadCommit(voice);
  if (result == 0) NoteBank_HardOff(voice);
  return result;
}

void NoteBank_VmUploadAbort(uint8_t voice)
{
  ChannelVm_UploadAbort(voice);
}

uint8_t NoteBank_VmUploadIsActive(uint8_t voice)
{
  return ChannelVm_UploadIsActive(voice);
}

uint8_t NoteBank_VmUploadIsBusy(void)
{
  return ChannelVm_UploadIsBusy();
}

const FwVmMemoryMetrics *NoteBank_VmMemoryMetrics(void)
{
  return ChannelVm_MemoryMetrics();
}

FwVmFault NoteBank_VmFault(uint8_t voice)
{
  return ChannelVm_Fault(voice);
}

uint32_t NoteBank_VmMaxCycles(uint8_t voice)
{
  const FwVmMetrics *metrics = ChannelVm_Metrics(voice);
  return metrics != NULL ? metrics->boundary_cycles_max : 0u;
}

uint32_t NoteBank_VmFaultCount(uint8_t voice)
{
  const FwVmMetrics *metrics = ChannelVm_Metrics(voice);
  return metrics != NULL ? metrics->faults : 0u;
}
