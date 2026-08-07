/**
 ******************************************************************************
 * @file    wave_bank.c
 * @brief   AXI-backed 8×32 KiB one-shot waveform banks + linear interp.
 ******************************************************************************
 */

#include "wave_bank.h"
#include "audio_rate.h"

#include <string.h>

typedef struct
{
  uint32_t length;   /* samples loaded */
  uint32_t pos_i;    /* integer playhead index */
  uint32_t pos_frac; /* Q32 fraction */
  uint32_t step_i;   /* integer step per output sample */
  uint32_t step_frac;
  uint8_t playing;
} WaveSlot_t;

/* Sample memory in AXI SRAM (DMA1-reachable domain; CPU-filled I2S rings
 * share this region). Not DTCM — banks are 256 KiB total. */
static int16_t s_data[WAVE_BANK_SLOTS][WAVE_BANK_SAMPLES_MAX]
    __attribute__((section(".wave_bank"), aligned(4)));

static WaveSlot_t s_slots[WAVE_BANK_SLOTS];

void WaveBank_Init(void)
{
  uint8_t i;

  for (i = 0u; i < WAVE_BANK_SLOTS; i++)
  {
    s_slots[i].length = 0u;
    s_slots[i].pos_i = 0u;
    s_slots[i].pos_frac = 0u;
    s_slots[i].step_i = 0u;
    s_slots[i].step_frac = 0u;
    s_slots[i].playing = 0u;
  }
  /* Leave s_data uninitialized (NOLOAD); length 0 means empty. */
}

void WaveBank_StopAll(void)
{
  uint8_t i;
  for (i = 0u; i < WAVE_BANK_SLOTS; i++)
  {
    WaveBank_Stop(i);
  }
}

int WaveBank_Load(uint8_t slot, const uint8_t *data, uint32_t nbytes)
{
  if (slot >= WAVE_BANK_SLOTS)
  {
    return -1;
  }
  if ((nbytes & 1u) != 0u || nbytes > WAVE_BANK_BYTES_MAX)
  {
    return -1;
  }
  if (nbytes > 0u && data == NULL)
  {
    return -1;
  }

  WaveBank_Stop(slot);
  if (nbytes > 0u)
  {
    memcpy(s_data[slot], data, nbytes);
  }
  s_slots[slot].length = nbytes / 2u;
  return 0;
}

int16_t *WaveBank_WritePtr(uint8_t slot)
{
  if (slot >= WAVE_BANK_SLOTS)
  {
    return NULL;
  }
  return s_data[slot];
}

int WaveBank_CommitLength(uint8_t slot, uint32_t nsamp)
{
  if (slot >= WAVE_BANK_SLOTS || nsamp > WAVE_BANK_SAMPLES_MAX)
  {
    return -1;
  }
  WaveBank_Stop(slot);
  s_slots[slot].length = nsamp;
  return 0;
}

uint32_t WaveBank_GetLength(uint8_t slot)
{
  if (slot >= WAVE_BANK_SLOTS)
  {
    return 0u;
  }
  return s_slots[slot].length;
}

double WaveBank_GetRate(uint8_t slot)
{
  if (slot >= WAVE_BANK_SLOTS || s_slots[slot].playing == 0u)
  {
    return 0.0;
  }
  /* Reconstruct approx rate from fixed-point step. */
  {
    uint64_t step =
        ((uint64_t)s_slots[slot].step_i << 32) | s_slots[slot].step_frac;
    return ((double)step * (double)AUDIO_SAMPLE_RATE_HZ) / 4294967296.0;
  }
}

uint8_t WaveBank_IsPlaying(uint8_t slot)
{
  if (slot >= WAVE_BANK_SLOTS)
  {
    return 0u;
  }
  return s_slots[slot].playing;
}

static int WaveBank_SetRate(uint8_t slot, double rate_hz)
{
  double step;
  uint64_t step_q32;

  if (rate_hz < WAVE_BANK_RATE_MIN || rate_hz > WAVE_BANK_RATE_MAX)
  {
    return -1;
  }

  /* samples advanced per output sample at AUDIO_SAMPLE_RATE_HZ */
  step = rate_hz / (double)AUDIO_SAMPLE_RATE_HZ;
  if (step <= 0.0)
  {
    return -1;
  }
  step_q32 = (uint64_t)(step * 4294967296.0 + 0.5);
  if (step_q32 == 0ull)
  {
    step_q32 = 1ull;
  }

  s_slots[slot].step_i = (uint32_t)(step_q32 >> 32);
  s_slots[slot].step_frac = (uint32_t)step_q32;
  return 0;
}

int WaveBank_Trigger(uint8_t slot, double rate_hz)
{
  if (slot >= WAVE_BANK_SLOTS)
  {
    return -1;
  }
  if (s_slots[slot].length < 2u)
  {
    return -1;
  }
  if (WaveBank_SetRate(slot, rate_hz) != 0)
  {
    return -1;
  }

  s_slots[slot].pos_i = 0u;
  s_slots[slot].pos_frac = 0u;
  s_slots[slot].playing = 1u;
  return 0;
}

void WaveBank_Stop(uint8_t slot)
{
  if (slot >= WAVE_BANK_SLOTS)
  {
    return;
  }
  s_slots[slot].playing = 0u;
  s_slots[slot].pos_i = 0u;
  s_slots[slot].pos_frac = 0u;
  s_slots[slot].step_i = 0u;
  s_slots[slot].step_frac = 0u;
}

int WaveBank_NoteOn(uint8_t slot, double freq_hz)
{
  if (freq_hz <= 0.0)
  {
    return -1;
  }
  return WaveBank_Trigger(slot, freq_hz * WAVE_BANK_RATE_PER_HZ);
}

uint8_t WaveBank_AnyPlaying(void)
{
  uint8_t i;
  for (i = 0u; i < WAVE_BANK_SLOTS; i++)
  {
    if (s_slots[i].playing != 0u)
    {
      return 1u;
    }
  }
  return 0u;
}

int32_t WaveBank_NextSample(uint8_t slot)
{
  WaveSlot_t *s;
  uint32_t i0;
  uint32_t i1;
  int32_t s0;
  int32_t s1;
  int32_t out;
  uint32_t frac;
  uint64_t pos;
  uint64_t step;

  if (slot >= WAVE_BANK_SLOTS)
  {
    return 0;
  }

  s = &s_slots[slot];
  if (s->playing == 0u || s->length < 2u)
  {
    return 0;
  }

  i0 = s->pos_i;
  if (i0 >= s->length)
  {
    s->playing = 0u;
    return 0;
  }

  i1 = i0 + 1u;
  s0 = ((int32_t)s_data[slot][i0]) << 16;
  if (i1 >= s->length)
  {
    /* Last sample: no interp partner — emit once then end. */
    out = s0;
    s->playing = 0u;
    s->pos_i = s->length;
    return out;
  }

  s1 = ((int32_t)s_data[slot][i1]) << 16;
  frac = s->pos_frac >> 16; /* Q16 for lerp */
  out = s0 + (int32_t)((((int64_t)s1 - (int64_t)s0) * (int64_t)frac) >> 16);

  step = ((uint64_t)s->step_i << 32) | (uint64_t)s->step_frac;
  pos = ((uint64_t)s->pos_i << 32) | (uint64_t)s->pos_frac;
  pos += step;
  s->pos_i = (uint32_t)(pos >> 32);
  s->pos_frac = (uint32_t)pos;

  if (s->pos_i >= s->length)
  {
    s->playing = 0u;
  }

  return out;
}
