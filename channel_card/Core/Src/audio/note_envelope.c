/**
 ******************************************************************************
 * @file    note_envelope.c
 * @brief   Multi-segment linear amp envelope (gate + per-seg pitch-scaled rate).
 *
 * Segments are (end, slope, k) chains; first note-on starts at 0. Last segment
 * is release to 0. Each segment's rate uses (f/C4)^k from that segment.
 ******************************************************************************
 */

#include "note_envelope.h"

#include "audio_rate.h"

#include <math.h>
#include <string.h>

typedef enum
{
  NOTE_ENV_IDLE = 0,
  NOTE_ENV_RUNNING = 1,
  NOTE_ENV_HOLD = 2,
  NOTE_ENV_RELEASE = 3
} NoteEnv_State_t;

typedef struct
{
  NoteEnv_Segment_t segs[NOTE_ENV_SEGMENTS_MAX];
  uint8_t n_segs;
  float default_k; /* last ek / Init; SetPitchK also writes all segs */
  NoteEnv_State_t state;
  uint8_t seg_idx;
  float amp;
  float target;
  float step;         /* signed Δamp per sample (cached) */
  float note_freq_hz; /* cached on NoteOn for Arm pitch_rate */
} NoteEnv_Voice_t;

static NoteEnv_Voice_t s_voices[NOTE_ENV_VOICES];

static uint8_t NoteEnv_ValidateK(float k)
{
  return (k >= NOTE_ENV_K_MIN && k <= NOTE_ENV_K_MAX) ? 1u : 0u;
}

static uint8_t NoteEnv_ValidatePre(const NoteEnv_Segment_t *s)
{
  if (s->end_amp < 0.0f || s->end_amp > 1.0f)
  {
    return 0u;
  }
  if (!(s->slope > 0.0f))
  {
    return 0u;
  }
  if (!NoteEnv_ValidateK(s->k))
  {
    return 0u;
  }
  return 1u;
}

static float NoteEnv_ComputePitchRate(float freq_hz, float k)
{
  float ratio;

  if (k == 0.0f)
  {
    return 1.0f;
  }
  if (!(freq_hz > 0.0f))
  {
    return 1.0f;
  }
  ratio = freq_hz / NOTE_ENV_F_REF_HZ;
  if (ratio <= 0.0f)
  {
    return 1.0f;
  }
  return powf(ratio, k);
}

/** Arm ramp from from_amp toward segs[idx].end at segment slope × pitch rate. */
static void NoteEnv_Arm(NoteEnv_Voice_t *v, uint8_t idx, float from_amp)
{
  const NoteEnv_Segment_t *seg = &v->segs[idx];
  float pitch_rate = NoteEnv_ComputePitchRate(v->note_freq_hz, seg->k);
  float step_mag =
      (seg->slope / (float)AUDIO_SAMPLE_RATE_HZ) * pitch_rate;

  v->seg_idx = idx;
  v->amp = from_amp;
  v->target = seg->end_amp;

  if (v->amp < v->target)
  {
    v->step = step_mag;
  }
  else if (v->amp > v->target)
  {
    v->step = -step_mag;
  }
  else
  {
    v->step = 0.0f;
  }
}

static void NoteEnv_EnterNextOrHold(NoteEnv_Voice_t *v)
{
  uint8_t release_idx = (uint8_t)(v->n_segs - 1u);
  uint8_t next = (uint8_t)(v->seg_idx + 1u);

  if (next >= release_idx)
  {
    v->state = NOTE_ENV_HOLD;
    v->step = 0.0f;
    return;
  }

  /* Continue from current amp (= previous segment end). */
  NoteEnv_Arm(v, next, v->amp);
  v->state = NOTE_ENV_RUNNING;
}

/* ---- public API --------------------------------------------------------- */

int NoteEnv_SetSegments(uint8_t voice, const NoteEnv_Segment_t *segs, uint8_t n)
{
  NoteEnv_Voice_t *v;
  uint8_t i;
  uint8_t release_idx;

  if (voice >= NOTE_ENV_VOICES || segs == NULL)
  {
    return -1;
  }
  if (n < NOTE_ENV_SEGMENTS_MIN || n > NOTE_ENV_SEGMENTS_MAX)
  {
    return -2;
  }

  release_idx = (uint8_t)(n - 1u);
  for (i = 0; i < release_idx; i++)
  {
    if (!NoteEnv_ValidatePre(&segs[i]))
    {
      return -2;
    }
  }
  if (!(segs[release_idx].slope > 0.0f) ||
      !NoteEnv_ValidateK(segs[release_idx].k))
  {
    return -2;
  }

  v = &s_voices[voice];
  memcpy(v->segs, segs, (size_t)n * sizeof(NoteEnv_Segment_t));
  /* Release always targets silence. */
  v->segs[release_idx].end_amp = 0.0f;
  v->n_segs = n;
  v->state = NOTE_ENV_IDLE;
  v->seg_idx = 0u;
  v->amp = 0.0f;
  v->target = 0.0f;
  v->step = 0.0f;
  return 0;
}

void NoteEnv_Clear(uint8_t voice)
{
  NoteEnv_Voice_t *v;

  if (voice >= NOTE_ENV_VOICES)
  {
    return;
  }
  v = &s_voices[voice];
  v->n_segs = 0u;
  v->state = NOTE_ENV_IDLE;
  v->seg_idx = 0u;
  v->amp = 0.0f;
  v->target = 0.0f;
  v->step = 0.0f;
  v->note_freq_hz = 0.0f;
}

void NoteEnv_Init(void)
{
  uint8_t i;

  for (i = 0u; i < NOTE_ENV_VOICES; i++)
  {
    NoteEnv_Clear(i);
    s_voices[i].default_k = 0.0f;
  }
}

uint8_t NoteEnv_IsProgrammed(uint8_t voice)
{
  if (voice >= NOTE_ENV_VOICES)
  {
    return 0u;
  }
  return (s_voices[voice].n_segs >= NOTE_ENV_SEGMENTS_MIN) ? 1u : 0u;
}

uint8_t NoteEnv_GetSegmentCount(uint8_t voice)
{
  if (voice >= NOTE_ENV_VOICES)
  {
    return 0u;
  }
  return s_voices[voice].n_segs;
}

int NoteEnv_GetSegment(uint8_t voice, uint8_t idx, NoteEnv_Segment_t *out)
{
  if (voice >= NOTE_ENV_VOICES || out == NULL)
  {
    return -1;
  }
  if (idx >= s_voices[voice].n_segs)
  {
    return -1;
  }
  *out = s_voices[voice].segs[idx];
  return 0;
}

int NoteEnv_SetPitchK(uint8_t voice, float k)
{
  NoteEnv_Voice_t *v;
  uint8_t i;

  if (voice >= NOTE_ENV_VOICES)
  {
    return -1;
  }
  if (!NoteEnv_ValidateK(k))
  {
    return -2;
  }
  v = &s_voices[voice];
  v->default_k = k;
  for (i = 0u; i < v->n_segs; i++)
  {
    v->segs[i].k = k;
  }
  return 0;
}

float NoteEnv_GetPitchK(uint8_t voice)
{
  NoteEnv_Voice_t *v;
  uint8_t i;
  float k0;

  if (voice >= NOTE_ENV_VOICES)
  {
    return 0.0f;
  }
  v = &s_voices[voice];
  if (v->n_segs < NOTE_ENV_SEGMENTS_MIN)
  {
    return v->default_k;
  }
  k0 = v->segs[0].k;
  for (i = 1u; i < v->n_segs; i++)
  {
    if (v->segs[i].k != k0)
    {
      return v->default_k;
    }
  }
  return k0;
}

void NoteEnv_NoteOn(uint8_t voice, float freq_hz)
{
  NoteEnv_Voice_t *v;

  if (voice >= NOTE_ENV_VOICES)
  {
    return;
  }
  v = &s_voices[voice];
  if (v->n_segs < NOTE_ENV_SEGMENTS_MIN)
  {
    return;
  }

  v->note_freq_hz = freq_hz;
  /* First segment always starts from silence. */
  NoteEnv_Arm(v, 0u, 0.0f);
  v->state = NOTE_ENV_RUNNING;

  if (v->step == 0.0f)
  {
    NoteEnv_EnterNextOrHold(v);
  }
}

void NoteEnv_NoteOff(uint8_t voice)
{
  NoteEnv_Voice_t *v;
  uint8_t release_idx;

  if (voice >= NOTE_ENV_VOICES)
  {
    return;
  }
  v = &s_voices[voice];
  if (v->n_segs < NOTE_ENV_SEGMENTS_MIN)
  {
    return;
  }
  if (v->state == NOTE_ENV_IDLE)
  {
    return;
  }

  release_idx = (uint8_t)(v->n_segs - 1u);
  NoteEnv_Arm(v, release_idx, v->amp);
  v->state = NOTE_ENV_RELEASE;

  if (v->step == 0.0f)
  {
    v->amp = v->target;
    v->state = NOTE_ENV_IDLE;
  }
}

void NoteEnv_Stop(uint8_t voice)
{
  NoteEnv_Voice_t *v;
  if (voice >= NOTE_ENV_VOICES)
  {
    return;
  }
  v = &s_voices[voice];
  v->state = NOTE_ENV_IDLE;
  v->seg_idx = 0u;
  v->amp = 0.0f;
  v->target = 0.0f;
  v->step = 0.0f;
}

uint8_t NoteEnv_IsActive(uint8_t voice)
{
  if (voice >= NOTE_ENV_VOICES)
  {
    return 0u;
  }
  if (s_voices[voice].n_segs < NOTE_ENV_SEGMENTS_MIN)
  {
    return 0u;
  }
  return (s_voices[voice].state != NOTE_ENV_IDLE) ? 1u : 0u;
}

float NoteEnv_Process(uint8_t voice)
{
  NoteEnv_Voice_t *v;
  float next;

  if (voice >= NOTE_ENV_VOICES)
  {
    return 1.0f;
  }
  v = &s_voices[voice];
  if (v->n_segs < NOTE_ENV_SEGMENTS_MIN)
  {
    return 1.0f;
  }

  if (v->state == NOTE_ENV_IDLE)
  {
    return 0.0f;
  }
  if (v->state == NOTE_ENV_HOLD)
  {
    return v->amp;
  }

  if (v->step == 0.0f)
  {
    v->amp = v->target;
  }
  else
  {
    next = v->amp + v->step;
    if (v->step > 0.0f)
    {
      if (next >= v->target)
      {
        v->amp = v->target;
        v->step = 0.0f;
      }
      else
      {
        v->amp = next;
      }
    }
    else
    {
      if (next <= v->target)
      {
        v->amp = v->target;
        v->step = 0.0f;
      }
      else
      {
        v->amp = next;
      }
    }
  }

  if (v->step == 0.0f && v->amp == v->target)
  {
    if (v->state == NOTE_ENV_RELEASE)
    {
      v->state = NOTE_ENV_IDLE;
      return 0.0f;
    }
    NoteEnv_EnterNextOrHold(v);
  }

  return v->amp;
}
