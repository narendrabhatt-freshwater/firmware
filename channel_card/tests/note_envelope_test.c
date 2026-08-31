#include "note_envelope.h"

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
  check(NoteEnv_StartRamp(0u, NAN, 1.0f) < 0 &&
            NoteEnv_StartRamp(0u, 1.0f, 0.0f) < 0,
        "invalid ramp rejected");

  puts("Native envelope ramp tests passed");
  return EXIT_SUCCESS;
}

