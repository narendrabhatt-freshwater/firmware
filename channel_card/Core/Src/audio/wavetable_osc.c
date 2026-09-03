/**
 ******************************************************************************
 * @file    wavetable_osc.c
 * @brief   Dynamically allocated signed-int8 wavetable oscillator instances.
 ******************************************************************************
 */

#include "wavetable_osc.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

#define OSC_PHASE_ONE (1u << 16)
#define OSC_HANDLE_MAX UINT32_C(0x00FFFFFF)

typedef struct WavetableOscEntry
{
  struct WavetableOscEntry *next;
  uint32_t phase;
  uint32_t increment;
  uint32_t handle;
  uint16_t length;
  uint8_t wave_id;
} WavetableOscEntry_t;

static WavetableOscEntry_t *s_active_head[SAMPLE_VOICES];
static WavetableOscEntry_t *s_pending_head[SAMPLE_VOICES];
static WavetableOscEntry_t *s_pending_tail[SAMPLE_VOICES];
static uint32_t s_next_handle = 1u;
static uint8_t s_handle_wrapped;

#if defined(__GNUC__)
#define WAVETABLE_OSC_WEAK __attribute__((weak))
#else
#define WAVETABLE_OSC_WEAK
#endif

/* Weak hooks let host tests inject allocation failure. Production uses the
 * card heap and therefore has no oscillator-instance count constant. */
WAVETABLE_OSC_WEAK void *WavetableOsc_Allocate(size_t size)
{
  return malloc(size);
}

WAVETABLE_OSC_WEAK void WavetableOsc_Deallocate(void *pointer)
{
  free(pointer);
}

static void WavetableOsc_ReleaseList(WavetableOscEntry_t **head)
{
  WavetableOscEntry_t *entry = *head;
  *head = NULL;
  while (entry != NULL)
  {
    WavetableOscEntry_t *next = entry->next;
    WavetableOsc_Deallocate(entry);
    entry = next;
  }
}

static uint8_t WavetableOsc_HandleInList(const WavetableOscEntry_t *entry,
                                         uint32_t handle)
{
  while (entry != NULL)
  {
    if (entry->handle == handle)
    {
      return 1u;
    }
    entry = entry->next;
  }
  return 0u;
}

static uint8_t WavetableOsc_HandleInUse(uint32_t handle)
{
  uint8_t voice;
  for (voice = 0u; voice < SAMPLE_VOICES; voice++)
  {
    if (WavetableOsc_HandleInList(s_active_head[voice], handle) != 0u ||
        WavetableOsc_HandleInList(s_pending_head[voice], handle) != 0u)
    {
      return 1u;
    }
  }
  return 0u;
}

static uint32_t WavetableOsc_NewHandle(void)
{
  uint32_t handle;
  uint32_t attempts = 0u;

  /* Until the 24-bit namespace wraps, the monotonically increasing value
   * cannot collide and allocation stays constant-time regardless of how many
   * oscillators already exist. The slow collision check is a wrap-only path. */
  if (s_handle_wrapped == 0u)
  {
    handle = s_next_handle++;
    if (s_next_handle > OSC_HANDLE_MAX)
    {
      s_next_handle = 1u;
      s_handle_wrapped = 1u;
    }
    return handle;
  }

  while (attempts < OSC_HANDLE_MAX)
  {
    handle = s_next_handle++;
    if (s_next_handle > OSC_HANDLE_MAX)
    {
      s_next_handle = 1u;
    }
    if (WavetableOsc_HandleInUse(handle) == 0u)
    {
      return handle;
    }
    attempts++;
  }
  return 0u;
}

void WavetableOsc_Init(void)
{
  WavetableOsc_StopAll();
  s_next_handle = 1u;
  s_handle_wrapped = 0u;
}

void WavetableOsc_BeginPending(uint8_t voice)
{
  if (voice < SAMPLE_VOICES)
  {
    WavetableOsc_DiscardPending(voice);
  }
}

int WavetableOsc_AddPending(uint8_t voice, uint8_t wave,
                            float frequency_hz, uint32_t *handle_out)
{
  uint16_t wave_id;
  uint32_t length;
  uint32_t handle;
  double increment;
  WavetableOscEntry_t *entry;

  if (voice >= SAMPLE_VOICES || wave >= WAVETABLE_OSC_WAVE_COUNT ||
      handle_out == NULL || !isfinite(frequency_hz) ||
      !(frequency_hz > 0.0f) || frequency_hz > WAVETABLE_OSC_MAX_HZ)
  {
    return -1;
  }
  wave_id = (uint16_t)(WAVETABLE_OSC_WAVE_FIRST + wave);
  length = AttackBank_GetLen(wave_id);
  if (length < 2u || length > ATTACK_BANK_LEN ||
      AttackBank_Table(wave_id) == NULL)
  {
    return -1;
  }

  increment = (double)frequency_hz * (double)length *
              (double)OSC_PHASE_ONE / (double)WAVETABLE_OSC_SAMPLE_RATE_HZ;
  if (increment < 1.0)
  {
    increment = 1.0;
  }

  entry = (WavetableOscEntry_t *)WavetableOsc_Allocate(sizeof(*entry));
  if (entry == NULL)
  {
    return -1;
  }
  handle = WavetableOsc_NewHandle();
  if (handle == 0u)
  {
    WavetableOsc_Deallocate(entry);
    return -1;
  }

  entry->next = NULL;
  entry->phase = 0u;
  entry->increment = (uint32_t)(increment + 0.5);
  entry->handle = handle;
  entry->length = (uint16_t)length;
  entry->wave_id = (uint8_t)wave_id;

  if (s_pending_tail[voice] == NULL)
  {
    s_pending_head[voice] = entry;
  }
  else
  {
    s_pending_tail[voice]->next = entry;
  }
  s_pending_tail[voice] = entry;
  *handle_out = handle;
  return 0;
}

void WavetableOsc_ActivatePending(uint8_t voice)
{
  WavetableOscEntry_t *entry;
  if (voice >= SAMPLE_VOICES)
  {
    return;
  }
  WavetableOsc_StopActive(voice);
  s_active_head[voice] = s_pending_head[voice];
  s_pending_head[voice] = NULL;
  s_pending_tail[voice] = NULL;
  entry = s_active_head[voice];
  while (entry != NULL)
  {
    entry->phase = 0u;
    entry = entry->next;
  }
}

void WavetableOsc_DiscardPending(uint8_t voice)
{
  if (voice < SAMPLE_VOICES)
  {
    WavetableOsc_ReleaseList(&s_pending_head[voice]);
    s_pending_tail[voice] = NULL;
  }
}

void WavetableOsc_StopActive(uint8_t voice)
{
  if (voice < SAMPLE_VOICES)
  {
    WavetableOsc_ReleaseList(&s_active_head[voice]);
  }
}

void WavetableOsc_Stop(uint8_t voice)
{
  if (voice < SAMPLE_VOICES)
  {
    WavetableOsc_StopActive(voice);
    WavetableOsc_DiscardPending(voice);
  }
}

void WavetableOsc_StopAll(void)
{
  uint8_t voice;
  for (voice = 0u; voice < SAMPLE_VOICES; voice++)
  {
    WavetableOsc_Stop(voice);
  }
}

uint8_t WavetableOsc_HandleIsValid(uint8_t voice, uint32_t handle)
{
  if (voice >= SAMPLE_VOICES || handle == 0u || handle > OSC_HANDLE_MAX)
  {
    return 0u;
  }
  return WavetableOsc_HandleInList(s_active_head[voice], handle) != 0u ||
                 WavetableOsc_HandleInList(s_pending_head[voice], handle) != 0u
             ? 1u
             : 0u;
}

static int32_t WavetableOsc_Next(WavetableOscEntry_t *entry)
{
  const int8_t *table = AttackBank_Table(entry->wave_id);
  uint32_t period = (uint32_t)entry->length << 16;
  uint32_t index = entry->phase >> 16;
  uint32_t next = index + 1u;
  uint32_t fraction = entry->phase & 0xFFFFu;
  int32_t first;
  int32_t second;
  int32_t sample;

  if (next >= entry->length)
  {
    next = 0u;
  }
  first = (int32_t)table[index] * 16777216;
  second = (int32_t)table[next] * 16777216;
  sample = (int32_t)((int64_t)first +
                     (((int64_t)second - (int64_t)first) * fraction >> 16));
  entry->phase += entry->increment;
  if (entry->phase >= period)
  {
    entry->phase -= period;
  }
  return sample;
}

int64_t WavetableOsc_NextSum(uint8_t voice, uint32_t *count_out)
{
  int64_t sum = 0;
  uint32_t count = 0u;
  WavetableOscEntry_t *entry;

  if (voice < SAMPLE_VOICES)
  {
    entry = s_active_head[voice];
    while (entry != NULL)
    {
      sum += WavetableOsc_Next(entry);
      entry = entry->next;
      count++;
    }
  }
  if (count_out != NULL)
  {
    *count_out = count;
  }
  return sum;
}
