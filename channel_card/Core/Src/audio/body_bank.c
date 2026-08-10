/**
 ******************************************************************************
 * @file    body_bank.c
 * @brief   AXI int16 sustain loops + root Hz for on-card pitch.
 ******************************************************************************
 */

#include "body_bank.h"

#include <math.h>
#include <string.h>

#ifndef BODY_DEFAULT_ROOT_HZ
#define BODY_DEFAULT_ROOT_HZ 261.625565f
#endif

static int16_t s_data[BODY_BANK_COUNT][BODY_BANK_LEN]
    __attribute__((section(".attack_bank"), aligned(4)));
static uint32_t s_len[BODY_BANK_COUNT];
static uint8_t s_loaded[BODY_BANK_COUNT];
static float s_root_hz[BODY_BANK_COUNT];

void BodyBank_Init(void)
{
  uint16_t i;
  memset(s_loaded, 0, sizeof(s_loaded));
  memset(s_len, 0, sizeof(s_len));
  for (i = 0u; i < BODY_BANK_COUNT; i++)
  {
    s_root_hz[i] = BODY_DEFAULT_ROOT_HZ;
  }
}

int BodyBank_Load(uint16_t wave_id, const uint8_t *data, uint32_t nbytes)
{
  uint32_t nsamp;
  if (wave_id >= BODY_BANK_COUNT || data == NULL)
  {
    return -1;
  }
  if ((nbytes < 2u) || ((nbytes & 1u) != 0u) || (nbytes > BODY_BANK_BYTES))
  {
    return -1;
  }
  nsamp = nbytes / 2u;
  memcpy(s_data[wave_id], data, nbytes);
  s_len[wave_id] = nsamp;
  s_loaded[wave_id] = 1u;
  return 0;
}

int16_t *BodyBank_WritePtr(uint16_t wave_id)
{
  if (wave_id >= BODY_BANK_COUNT)
  {
    return NULL;
  }
  return s_data[wave_id];
}

int BodyBank_Commit(uint16_t wave_id, uint32_t nsamp)
{
  if (wave_id >= BODY_BANK_COUNT || nsamp == 0u || nsamp > BODY_BANK_LEN)
  {
    return -1;
  }
  s_len[wave_id] = nsamp;
  s_loaded[wave_id] = 1u;
  return 0;
}

uint8_t BodyBank_IsLoaded(uint16_t wave_id)
{
  if (wave_id >= BODY_BANK_COUNT)
  {
    return 0u;
  }
  return s_loaded[wave_id];
}

uint32_t BodyBank_Length(uint16_t wave_id)
{
  if (wave_id >= BODY_BANK_COUNT || s_loaded[wave_id] == 0u)
  {
    return 0u;
  }
  return s_len[wave_id];
}

int32_t BodyBank_SampleAt(uint16_t wave_id, double phase)
{
  uint32_t n;
  uint32_t i0;
  uint32_t i1;
  double frac;
  double a;
  double b;
  double y;

  if (wave_id >= BODY_BANK_COUNT || s_loaded[wave_id] == 0u)
  {
    return 0;
  }
  n = s_len[wave_id];
  if (n == 0u)
  {
    return 0;
  }
  if (n == 1u)
  {
    return ((int32_t)s_data[wave_id][0]) << 16;
  }

  if (phase >= (double)n || phase < 0.0)
  {
    phase = fmod(phase, (double)n);
    if (phase < 0.0)
    {
      phase += (double)n;
    }
  }
  i0 = (uint32_t)phase;
  i1 = i0 + 1u;
  if (i1 >= n)
  {
    i1 = 0u;
  }
  frac = phase - (double)i0;
  a = (double)s_data[wave_id][i0];
  b = (double)s_data[wave_id][i1];
  y = a + (b - a) * frac;
  if (y > 32767.0)
  {
    y = 32767.0;
  }
  else if (y < -32768.0)
  {
    y = -32768.0;
  }
  return ((int32_t)lround(y)) << 16;
}

void BodyBank_SetRootHz(uint16_t wave_id, float root_hz)
{
  if (wave_id >= BODY_BANK_COUNT || !(root_hz > 0.0f) || !isfinite(root_hz))
  {
    return;
  }
  s_root_hz[wave_id] = root_hz;
}

float BodyBank_GetRootHz(uint16_t wave_id)
{
  if (wave_id >= BODY_BANK_COUNT)
  {
    return BODY_DEFAULT_ROOT_HZ;
  }
  return s_root_hz[wave_id];
}
