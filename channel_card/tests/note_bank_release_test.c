#include "note_bank.h"
#include "stream_ring.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int16_t s_old_body[3000];
static const int16_t s_attack[] = {0, 12000, 6000, 0};
static uint32_t s_attack_len;

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
  return sample;
}

void NoteFilter_Reset(uint8_t voice) { (void)voice; }

void NoteFilter_OnNoteFreq(uint8_t voice, double hz)
{
  (void)voice;
  (void)hz;
}

uint8_t NoteEnv_IsProgrammed(uint8_t voice)
{
  (void)voice;
  return 0u;
}

uint8_t NoteEnv_IsActive(uint8_t voice)
{
  (void)voice;
  return 0u;
}

void NoteEnv_NoteOn(uint8_t voice, float hz)
{
  (void)voice;
  (void)hz;
}

void NoteEnv_NoteOff(uint8_t voice) { (void)voice; }

float NoteEnv_Process(uint8_t voice)
{
  (void)voice;
  return 1.0f;
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
  NoteBank_SetCrashReleaseMs(0u);
  NoteBank_SetFreqSession(0u, 260.0, 0.125, 2u);
  Check(NoteBank_NextSample() == 0,
        "0 ms replacement must start at attack sample zero immediately");
  Check(NoteBank_NextSample() > 0,
        "0 ms replacement must advance on the next output sample");

  puts("PASS: release reaches zero before replacement attack phase zero");
  return EXIT_SUCCESS;
}
