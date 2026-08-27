#include "note_bank.h"
#include "stream_ring.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int16_t s_old_body[3000];
static const int16_t s_attack[] = {0, 12000, 6000, 0};
static uint32_t s_attack_len;
static unsigned s_filter_divisor = 1u;
static unsigned s_filter_process_calls;
static unsigned s_filter_reset_calls;
static unsigned s_filter_note_on_calls;
static double s_filter_last_hz;
static uint8_t s_env_programmed;
static uint8_t s_env_active;
static float s_env_gain = 1.0f;
static unsigned s_env_process_calls;
static unsigned s_env_note_on_calls;
static unsigned s_env_stop_calls;

static void Check(int ok, const char *message)
{
  if (!ok)
  {
    fprintf(stderr, "%s\n", message);
    exit(EXIT_FAILURE);
  }
}

float AttackBank_GetRootHz(uint16_t wave_id)
{
  (void)wave_id;
  return 260.0f;
}

uint32_t AttackBank_GetLen(uint16_t wave_id)
{
  (void)wave_id;
  return s_attack_len;
}

const int16_t *AttackBank_Table(uint16_t wave_id)
{
  (void)wave_id;
  return s_attack;
}

void AttackBank_Stop(uint8_t voice) { (void)voice; }
void AttackBank_StopAll(void) {}

int32_t NoteFilter_Process(uint8_t voice, int32_t sample)
{
  (void)voice;
  s_filter_process_calls++;
  return sample / (int32_t)s_filter_divisor;
}

void NoteFilter_Reset(uint8_t voice)
{
  (void)voice;
  s_filter_reset_calls++;
}

void NoteFilter_OnNoteFreq(uint8_t voice, double hz)
{
  (void)voice;
  s_filter_note_on_calls++;
  s_filter_last_hz = hz;
}

uint8_t NoteEnv_IsProgrammed(uint8_t voice)
{
  (void)voice;
  return (s_env_programmed != 0u && s_env_active != 0u) ? 1u : 0u;
}

uint8_t NoteEnv_IsActive(uint8_t voice)
{
  (void)voice;
  return s_env_programmed;
}

void NoteEnv_NoteOn(uint8_t voice, float hz)
{
  (void)voice;
  (void)hz;
  s_env_note_on_calls++;
  s_env_active = s_env_programmed;
}

void NoteEnv_NoteOff(uint8_t voice) { (void)voice; }

void NoteEnv_Stop(uint8_t voice)
{
  (void)voice;
  s_env_active = 0u;
  s_env_stop_calls++;
}

float NoteEnv_Process(uint8_t voice)
{
  (void)voice;
  s_env_process_calls++;
  return s_env_gain;
}

static void StartOldVoice(void)
{
  uint32_t i;
  for (i = 0u; i < 3000u; ++i)
  {
    s_old_body[i] = 12000;
  }
  s_attack_len = 0u;
  StreamRing_Init();
  NoteBank_Init();
  Check(NoteBank_SetWaveId(0u, 0u) == 0, "assign wave");
  NoteBank_SetFreqSession(0u, 260.0, 0.125, 1u);
  (void)NoteBank_NextSample();
  Check(StreamRing_WriteVoice(0u, 1u, 1u, 0u, s_old_body, 3000u) ==
            3000u,
        "publish old BODY");
  Check(NoteBank_NextSample() > 0, "old BODY must be audible");
}

int main(void)
{
  unsigned i;
  int32_t previous;
  int32_t sample;

  StartOldVoice();
  s_attack_len = sizeof s_attack / sizeof s_attack[0];
  NoteBank_SetCrashReleaseMs(2u);
  NoteBank_SetFreqSession(0u, 260.0, 0.125, 2u);
  previous = NoteBank_NextSample();
  Check(previous > 0, "release must begin from the old signal");
  for (i = 1u; i < 96u; ++i)
  {
    sample = NoteBank_NextSample();
    Check(sample > 0, "2 ms release ended before 96 samples");
    Check(sample <= previous, "release must fade monotonically");
    previous = sample;
  }
  Check(NoteBank_NextSample() == 0,
        "replacement must start from attack sample zero after 2 ms");
  Check(NoteBank_NextSample() > 0,
        "replacement attack must advance only after the release");

  StartOldVoice();
  s_attack_len = sizeof s_attack / sizeof s_attack[0];
  NoteBank_SetCrashReleaseMs(6u);
  NoteBank_SetFreqSession(0u, 260.0, 0.125, 2u);
  previous = NoteBank_NextSample();
  Check(previous > 0, "6 ms release must begin from the old signal");
  for (i = 1u; i < 288u; ++i)
  {
    sample = NoteBank_NextSample();
    Check(sample > 0, "6 ms release ended before 288 samples");
    Check(sample <= previous, "6 ms release must fade monotonically");
    previous = sample;
  }
  Check(NoteBank_NextSample() == 0,
        "replacement must start from attack sample zero after 6 ms");
  Check(NoteBank_NextSample() > 0,
        "6 ms replacement must advance after exactly 288 release samples");

  StartOldVoice();
  s_attack_len = sizeof s_attack / sizeof s_attack[0];
  NoteBank_SetCrashReleaseMs(0u);
  NoteBank_SetFreqSession(0u, 260.0, 0.125, 2u);
  Check(NoteBank_NextSample() == 0,
        "0 ms replacement must start at attack sample zero immediately");
  Check(NoteBank_NextSample() > 0,
        "0 ms replacement must advance on the next output sample");

  s_env_programmed = 1u;
  s_env_gain = 0.5f;
  s_filter_divisor = 2u;
  StartOldVoice();
  sample = NoteBank_NextSample();
  {
    const int32_t old_output = sample;
    const unsigned filter_reset_before = s_filter_reset_calls;
    const unsigned filter_note_on_before = s_filter_note_on_calls;
    const unsigned env_note_on_before = s_env_note_on_calls;
    const unsigned filter_process_before = s_filter_process_calls;
    const unsigned env_process_before = s_env_process_calls;

    NoteBank_SetCrashReleaseMs(2u);
    s_attack_len = sizeof s_attack / sizeof s_attack[0];
    NoteBank_SetFreqSession(0u, 520.0, 0.25, 2u);
    Check(NoteBank_NextSample() == old_output,
          "release must begin continuously after old filter/envelope output");
    Check(s_filter_process_calls == filter_process_before + 1u &&
              s_env_process_calls == env_process_before + 1u,
          "old filter and envelope must process every release sample");
    Check(s_filter_reset_calls == filter_reset_before &&
              s_filter_note_on_calls == filter_note_on_before &&
              s_env_note_on_calls == env_note_on_before,
          "replacement DSP state must remain deferred during release");
    NoteBank_SetFreqSession(0u, 780.0, 0.5, 3u);
    (void)NoteBank_NextSample();
    Check(s_filter_reset_calls == filter_reset_before &&
              s_filter_note_on_calls == filter_note_on_before &&
              s_env_note_on_calls == env_note_on_before,
          "newer replacement must only supersede staged state");
    for (i = 2u; i < 95u; ++i)
    {
      (void)NoteBank_NextSample();
    }
    Check(s_filter_reset_calls == filter_reset_before &&
              s_filter_note_on_calls == filter_note_on_before &&
              s_env_note_on_calls == env_note_on_before,
          "replacement DSP state must stay untouched before the final frame");
    Check(NoteBank_NextSample() > 0,
          "final old DSP release sample must precede replacement activation");
    Check(s_filter_reset_calls == filter_reset_before + 1u &&
              s_filter_note_on_calls == filter_note_on_before + 1u &&
              s_env_note_on_calls == env_note_on_before + 1u,
          "replacement filter/envelope must activate exactly at release end");
    Check(s_filter_last_hz == 780.0,
          "latest replacement must own deferred DSP activation");
    Check(NoteBank_NextSample() == 0,
          "filtered/enveloped replacement must still start at attack phase zero");
  }

  StartOldVoice();
  {
    const unsigned env_note_on_before = s_env_note_on_calls;
    const unsigned env_stop_before = s_env_stop_calls;
    NoteBank_SetCrashReleaseMs(2u);
    s_attack_len = sizeof s_attack / sizeof s_attack[0];
    NoteBank_SetFreqSession(0u, 520.0, 0.25, 2u);
    Check(NoteBank_NextSample() > 0,
          "replacement release must begin before cancellation");
    NoteBank_SetFreqSession(0u, 0.0, 0.0, 3u);
    Check(NoteBank_NextSample() == 0 && NoteBank_IsActive(0u) == 0u,
          "note-off must cancel a staged replacement immediately");
    Check(s_env_note_on_calls == env_note_on_before &&
              s_env_stop_calls == env_stop_before + 1u &&
              NoteBank_NextSample() == 0,
          "canceled replacement must never retrigger its envelope");
  }

  puts("PASS: old DSP releases before replacement DSP/attack phase zero");
  return EXIT_SUCCESS;
}
