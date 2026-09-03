/**
 ******************************************************************************
 * @file    attack_bank.c
 * @brief   AXI signed-int8 attack heads; rate-scaled playheads (n0 pitch).
 ******************************************************************************
 */

#include "attack_bank.h"

#include <math.h>
#include <string.h>

#ifndef ATTACK_DEFAULT_ROOT_HZ
#define ATTACK_DEFAULT_ROOT_HZ 261.625565f
#endif

typedef struct
{
  uint16_t wave_id;
  float phase;
  float phase_inc;
  uint8_t playing;
} AttackVoice_t;

#if defined(__APPLE__)
#define ATTACK_BANK_SECTION __attribute__((used, aligned(4)))
#else
#define ATTACK_BANK_SECTION \
  __attribute__((used, section(".attack_bank"), aligned(4)))
#endif

static int8_t s_data[ATTACK_BANK_COUNT][ATTACK_BANK_LEN]
    ATTACK_BANK_SECTION;

static uint8_t s_loaded[ATTACK_BANK_COUNT];
static uint32_t s_len[ATTACK_BANK_COUNT];
static float s_root_hz[ATTACK_BANK_COUNT];
static AttackVoice_t s_voices[SAMPLE_VOICES];
static volatile uint8_t s_write_active;

static int32_t AttackBank_S8ToQ31(int8_t s)
{
  return (int32_t)s * 16777216;
}

void AttackBank_Init(void)
{
  uint16_t i;

  memset(s_loaded, 0, sizeof(s_loaded));
  memset(s_len, 0, sizeof(s_len));
  s_write_active = 0u;
  for (i = 0u; i < ATTACK_BANK_COUNT; i++)
  {
    s_root_hz[i] = ATTACK_DEFAULT_ROOT_HZ;
  }
  for (i = 0u; i < SAMPLE_VOICES; i++)
  {
    s_voices[i].playing = 0u;
    s_voices[i].phase = 0.0f;
    s_voices[i].phase_inc = 1.0f;
    s_voices[i].wave_id = 0u;
  }
}

void AttackBank_SetWriteActive(uint8_t active)
{
  s_write_active = active != 0u ? 1u : 0u;
}

uint8_t AttackBank_WriteIsActive(void)
{
  return s_write_active;
}

int AttackBank_Load(uint16_t wave_id, const uint8_t *data, uint32_t nbytes)
{
  if (wave_id >= ATTACK_BANK_COUNT || data == NULL ||
      nbytes == 0u || nbytes > ATTACK_BANK_BYTES)
  {
    return -1;
  }
  memcpy(s_data[wave_id], data, nbytes);
  if (nbytes < ATTACK_BANK_BYTES)
  {
    memset((uint8_t *)s_data[wave_id] + nbytes, 0,
           ATTACK_BANK_BYTES - nbytes);
  }
  s_len[wave_id] = nbytes;
  s_loaded[wave_id] = 1u;
  return 0;
}

int8_t *AttackBank_WritePtr(uint16_t wave_id)
{
  if (wave_id >= ATTACK_BANK_COUNT)
  {
    return NULL;
  }
  return s_data[wave_id];
}

const int8_t *AttackBank_Table(uint16_t wave_id)
{
  if (wave_id >= ATTACK_BANK_COUNT)
  {
    return NULL;
  }
  return s_data[wave_id];
}

int AttackBank_Commit(uint16_t wave_id, uint32_t nsamp)
{
  if (wave_id >= ATTACK_BANK_COUNT || nsamp == 0u ||
      nsamp > ATTACK_BANK_LEN)
  {
    return -1;
  }
  s_len[wave_id] = nsamp;
  s_loaded[wave_id] = 1u;
  return 0;
}

uint32_t AttackBank_GetLen(uint16_t wave_id)
{
  if (wave_id >= ATTACK_BANK_COUNT || s_loaded[wave_id] == 0u)
  {
    return 0u;
  }
  return s_len[wave_id];
}

uint8_t AttackBank_IsLoaded(uint16_t wave_id)
{
  if (wave_id >= ATTACK_BANK_COUNT)
  {
    return 0u;
  }
  return s_loaded[wave_id];
}

uint16_t AttackBank_LoadedCount(void)
{
  uint16_t i;
  uint16_t n = 0u;
  for (i = 0u; i < ATTACK_BANK_COUNT; i++)
  {
    if (s_loaded[i] != 0u)
    {
      n++;
    }
  }
  return n;
}

void AttackBank_LoadedMask(uint8_t *out)
{
  uint16_t i;
  if (out == NULL)
  {
    return;
  }
  memset(out, 0, 32);
  for (i = 0u; i < ATTACK_BANK_COUNT; i++)
  {
    if (s_loaded[i] != 0u)
    {
      out[i >> 3] = (uint8_t)(out[i >> 3] | (uint8_t)(1u << (i & 7u)));
    }
  }
}

void AttackBank_SetRootHz(uint16_t wave_id, float root_hz)
{
  if (wave_id >= ATTACK_BANK_COUNT || !(root_hz > 0.0f) || !isfinite(root_hz))
  {
    return;
  }
  s_root_hz[wave_id] = root_hz;
}

float AttackBank_GetRootHz(uint16_t wave_id)
{
  if (wave_id >= ATTACK_BANK_COUNT)
  {
    return ATTACK_DEFAULT_ROOT_HZ;
  }
  return s_root_hz[wave_id];
}

int AttackBank_NoteOn(uint8_t voice, uint16_t wave_id, float phase_inc)
{
  if (voice >= SAMPLE_VOICES || wave_id >= ATTACK_BANK_COUNT ||
      s_loaded[wave_id] == 0u)
  {
    return -1;
  }
  if (!(phase_inc > 0.0f) || !isfinite(phase_inc))
  {
    phase_inc = 1.0f;
  }
  if (phase_inc > 16.0f)
  {
    phase_inc = 16.0f;
  }
  if (phase_inc < (1.0f / 16.0f))
  {
    phase_inc = 1.0f / 16.0f;
  }
  s_voices[voice].wave_id = wave_id;
  s_voices[voice].phase = 0.0f;
  s_voices[voice].phase_inc = phase_inc;
  s_voices[voice].playing = 1u;
  return 0;
}

void AttackBank_Stop(uint8_t voice)
{
  if (voice < SAMPLE_VOICES)
  {
    s_voices[voice].playing = 0u;
    s_voices[voice].phase = 0.0f;
  }
}

void AttackBank_StopAll(void)
{
  uint8_t i;
  for (i = 0u; i < SAMPLE_VOICES; i++)
  {
    AttackBank_Stop(i);
  }
}

uint8_t AttackBank_IsPlaying(uint8_t voice)
{
  if (voice >= SAMPLE_VOICES)
  {
    return 0u;
  }
  return s_voices[voice].playing;
}

int32_t AttackBank_NextSample(uint8_t voice)
{
  AttackVoice_t *v;
  uint16_t wid;
  float ph;
  uint32_t i0;
  uint32_t i1;
  uint32_t n;
  float frac;
  double a;
  double b;
  double y;

  if (voice >= SAMPLE_VOICES)
  {
    return 0;
  }
  v = &s_voices[voice];
  if (v->playing == 0u)
  {
    return 0;
  }

  wid = v->wave_id;
  n = s_len[wid];
  if (n == 0u)
  {
    n = ATTACK_BANK_LEN;
  }
  ph = v->phase;
  if (ph >= (float)n)
  {
    v->playing = 0u;
    v->phase = 0.0f;
    return AttackBank_S8ToQ31(s_data[wid][n - 1u]);
  }

  i0 = (uint32_t)ph;
  if (i0 >= n)
  {
    v->playing = 0u;
    return AttackBank_S8ToQ31(s_data[wid][n - 1u]);
  }
  i1 = i0 + 1u;
  frac = ph - (float)i0;
  a = (double)AttackBank_S8ToQ31(s_data[wid][i0]);
  if (i1 < n)
  {
    b = (double)AttackBank_S8ToQ31(s_data[wid][i1]);
    y = a + (b - a) * (double)frac;
  }
  else
  {
    y = a;
  }

  ph += v->phase_inc;
  v->phase = ph;
  if (ph >= (float)n)
  {
    v->playing = 0u;
  }

  return (int32_t)lround(y);
}

int32_t AttackBank_SampleAt(uint16_t wave_id, uint32_t index)
{
  if (wave_id >= ATTACK_BANK_COUNT || index >= ATTACK_BANK_LEN ||
      s_loaded[wave_id] == 0u)
  {
    return 0;
  }
  return AttackBank_S8ToQ31(s_data[wave_id][index]);
}
