#include "note_envelope.h"

#include "audio_rate.h"

#include <math.h>
#include <string.h>

typedef struct {
  float amplitude;
  float target;
  float step;
  uint8_t ramp_active;
  uint8_t ramp_end_pending;
} NoteEnvVoice;

static NoteEnvVoice s_voice[NOTE_ENV_VOICE_COUNT];

void NoteEnv_Init(void)
{
  memset(s_voice, 0, sizeof(s_voice));
}

void NoteEnv_Stop(uint8_t voice)
{
  if (voice >= NOTE_ENV_VOICE_COUNT) return;
  memset(&s_voice[voice], 0, sizeof(s_voice[voice]));
}

int NoteEnv_SetAmplitude(uint8_t voice, float amplitude)
{
  NoteEnvVoice *envelope;
  if (voice >= NOTE_ENV_VOICE_COUNT || !isfinite(amplitude) ||
      amplitude < 0.0f || amplitude > 1.0f) return -1;
  envelope = &s_voice[voice];
  envelope->amplitude = amplitude;
  envelope->target = amplitude;
  envelope->step = 0.0f;
  envelope->ramp_active = 0u;
  envelope->ramp_end_pending = 0u;
  return 0;
}

int NoteEnv_StartRamp(uint8_t voice, float target, float slope)
{
  NoteEnvVoice *envelope;
  float step;
  if (voice >= NOTE_ENV_VOICE_COUNT || !isfinite(target) ||
      !isfinite(slope) || target < 0.0f || target > 1.0f ||
      !(slope > 0.0f)) return -1;
  step = slope / (float)AUDIO_SAMPLE_RATE_HZ;
  if (!isfinite(step) || !(step > 0.0f)) return -1;
  envelope = &s_voice[voice];
  envelope->target = target;
  envelope->step = envelope->amplitude < target
                       ? step
                       : (envelope->amplitude > target ? -step : 0.0f);
  envelope->ramp_active = 1u;
  envelope->ramp_end_pending = 0u;
  return 0;
}

int NoteEnv_Hold(uint8_t voice)
{
  NoteEnvVoice *envelope;
  if (voice >= NOTE_ENV_VOICE_COUNT) return -1;
  envelope = &s_voice[voice];
  envelope->target = envelope->amplitude;
  envelope->step = 0.0f;
  envelope->ramp_active = 0u;
  envelope->ramp_end_pending = 0u;
  return 0;
}

float NoteEnv_Amplitude(uint8_t voice)
{
  return voice < NOTE_ENV_VOICE_COUNT ? s_voice[voice].amplitude : 0.0f;
}

float NoteEnv_RenderSample(uint8_t voice)
{
  NoteEnvVoice *envelope;
  float next;
  float tolerance;
  if (voice >= NOTE_ENV_VOICE_COUNT) return 0.0f;
  envelope = &s_voice[voice];
  if (envelope->ramp_active == 0u) return envelope->amplitude;
  next = envelope->amplitude + envelope->step;
  tolerance = fabsf(envelope->step) * 0.001f + 0.0000001f;
  if (envelope->step == 0.0f ||
      (envelope->step > 0.0f && next + tolerance >= envelope->target) ||
      (envelope->step < 0.0f && next - tolerance <= envelope->target)) {
    envelope->amplitude = envelope->target;
    envelope->step = 0.0f;
    envelope->ramp_active = 0u;
    envelope->ramp_end_pending = 1u;
  } else {
    envelope->amplitude = next;
  }
  return envelope->amplitude;
}

uint8_t NoteEnv_TakeRampEnd(uint8_t voice)
{
  uint8_t pending;
  if (voice >= NOTE_ENV_VOICE_COUNT) return 0u;
  pending = s_voice[voice].ramp_end_pending;
  s_voice[voice].ramp_end_pending = 0u;
  return pending;
}
