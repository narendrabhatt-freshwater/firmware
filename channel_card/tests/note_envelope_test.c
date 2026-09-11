#include "note_envelope.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void check(int condition, const char *message)
{
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(EXIT_FAILURE);
  }
}

static void clamped_amplitudes(void)
{
  static const float values[] = {-FLT_MAX, -200.0f, 0.0f, 0.5f, 1.0f, 200.0f, FLT_MAX};
  static const float expected[] = {0.0f, 0.0f, 0.0f, 0.5f, 1.0f, 1.0f, 1.0f};
  for (unsigned voice = 0u; voice < NOTE_ENV_VOICE_COUNT; ++voice) {
    for (unsigned i = 0u; i < sizeof values / sizeof values[0]; ++i) {
      check(NoteEnv_SetAmplitude(voice, values[i]) == 0, "finite amplitude accepted");
      check(NoteEnv_Amplitude(voice) == expected[i], "set amplitude clamped");
      check(NoteEnv_SetAmplitude(voice, 1.0f - expected[i]) == 0, "set ramp start");
      check(NoteEnv_StartRamp(voice, values[i], 48000.0f) == 0, "finite target accepted");
      check(NoteEnv_RenderSample(voice) == expected[i], "ramp target clamped");
      check(NoteEnv_TakeRampEnd(voice) == 1u && NoteEnv_TakeRampEnd(voice) == 0u,
            "clamped ramp completes once");
      check(NoteEnv_StartRamp(voice, values[i], 1.0f) == 0, "equal clamped target accepted");
      check(NoteEnv_TakeRampEnd(voice) == 1u && NoteEnv_TakeRampEnd(voice) == 0u,
            "equal clamped target completes once");
    }
  }
  check(NoteEnv_SetAmplitude(0u, NAN) < 0 &&
        NoteEnv_SetAmplitude(0u, INFINITY) < 0 &&
        NoteEnv_SetAmplitude(0u, -INFINITY) < 0 &&
        NoteEnv_StartRamp(0u, INFINITY, 1.0f) < 0 &&
        NoteEnv_StartRamp(0u, -INFINITY, 1.0f) < 0,
        "nonfinite amplitudes still rejected");
  check(NoteEnv_StartRamp(0u, 200.0f, NAN) < 0 &&
        NoteEnv_StartRamp(0u, 200.0f, INFINITY) < 0,
        "nonfinite slopes still rejected");
  check(NoteEnv_Amplitude(0u) == 1.0f, "invalid calls preserve amplitude");
}

static void slope_boundaries(void)
{
  static const float slopes[] = {0.0f, -0.0f, -8000.0f, FLT_MAX, -FLT_MAX};
  for (unsigned i = 0u; i < sizeof slopes / sizeof slopes[0]; ++i) {
    for (unsigned down = 0u; down < 2u; ++down) {
      float target = down ? 0.0f : 1.0f;
      check(NoteEnv_SetAmplitude(0u, 1.0f - target) == 0, "reset slope test");
      check(NoteEnv_StartRamp(0u, down ? -200.0f : 200.0f, slopes[i]) == 0,
            "zero, negative and extreme slopes accepted");
      if (slopes[i] == 0.0f)
        check(NoteEnv_Amplitude(0u) == target, "zero slope is instant");
      else if (fabsf(slopes[i]) > 48000.0f)
        check(NoteEnv_RenderSample(0u) == target, "huge slope completes in one sample");
      else {
        float first = NoteEnv_RenderSample(0u);
        check(first > 0.0f && first < 1.0f, "negative slope preserves speed");
      }
      for (unsigned sample = 0u; sample < 48u; ++sample) NoteEnv_RenderSample(0u);
      check(NoteEnv_Amplitude(0u) == target, "target determines ramp direction");
      check(NoteEnv_TakeRampEnd(0u) == 1u && NoteEnv_TakeRampEnd(0u) == 0u,
            "completion delivered once");
    }
  }
  check(NoteEnv_StartRamp(0u, 1.0f, nextafterf(0.0f, 1.0f)) == 0,
        "tiny finite slope does not fault");
  check(NoteEnv_RenderSample(0u) < 1.0f, "tiny slope does not become instant");
  check(NoteEnv_StartRamp(0u, 1.0f, 0.0f) == 0 && NoteEnv_TakeRampEnd(0u) == 1u,
        "instant ramp can replace slow ramp");
}

int main(void)
{
  unsigned sample;
  unsigned voice;
  NoteEnv_Init();

  for (voice = 0u; voice < NOTE_ENV_VOICE_COUNT; ++voice) {
    check(NoteEnv_SetAmplitude((uint8_t)voice, 0.0f) == 0, "set amplitude");
    check(NoteEnv_StartRamp((uint8_t)voice, 1.0f, 8000.0f) == 0,
          "start ramp");
  }
  for (sample = 0u; sample < 48u; ++sample)
    for (voice = 0u; voice < NOTE_ENV_VOICE_COUNT; ++voice)
      (void)NoteEnv_RenderSample((uint8_t)voice);

  for (voice = 0u; voice < NOTE_ENV_VOICE_COUNT; ++voice) {
    check(fabsf(NoteEnv_Amplitude((uint8_t)voice) - 1.0f) < 1e-6f,
          "target held");
    check(NoteEnv_TakeRampEnd((uint8_t)voice) == 1u,
          "one completion per voice");
    check(NoteEnv_TakeRampEnd((uint8_t)voice) == 0u,
          "completion consumed once");
  }

  check(NoteEnv_StartRamp(0u, 0.0f, 1.0f) == 0, "interrupt ramp");
  (void)NoteEnv_RenderSample(0u);
  check(NoteEnv_SetAmplitude(0u, 0.5f) == 0, "interrupt set");
  for (sample = 0u; sample < 48u; ++sample)
    (void)NoteEnv_RenderSample(0u);
  check(NoteEnv_TakeRampEnd(0u) == 0u,
        "interrupted ramp has no completion");

  check(NoteEnv_StartRamp(0u, 0.5f, 2.0f) == 0, "zero-distance ramp");
  (void)NoteEnv_RenderSample(0u);
  check(NoteEnv_TakeRampEnd(0u) == 1u,
        "zero-distance ramp completes once");
  check(NoteEnv_StartRamp(0u, NAN, 1.0f) < 0,
        "invalid ramp rejected");

  clamped_amplitudes();
  slope_boundaries();
  puts("Native envelope ramp tests passed");
  return EXIT_SUCCESS;
}
