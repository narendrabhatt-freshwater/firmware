/**
 ******************************************************************************
 * @file    wavetable_osc.c
 * @brief   Dynamically allocated signed-int8 wavetable oscillator instances.
 ******************************************************************************
 */

#include "wavetable_osc.h"

#include "freshwater/vm_channel.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

#define OSC_PHASE_ONE (1u << 16)
#define OSC_HANDLE_MAX UINT32_C(0x00FFFFFF)

typedef struct WavetableOscEntry
{
  struct WavetableOscEntry *next;
  struct WavetableOscEntry *render_next;
  uint32_t phase;
  uint32_t increment;
  uint32_t handle;
  float frequency_hz;
  float frequency_mod_hz;
  float amplitude_gain;
  int32_t rendered_sample;
  uint16_t length;
  uint8_t wave_id;
  uint8_t explicit_route;
  uint8_t ordered;
  uint32_t dependency_count;
} WavetableOscEntry_t;

typedef struct WavetableOscRoute
{
  struct WavetableOscRoute *next;
  WavetableOscEntry_t *source;
  WavetableOscEntry_t *target;
  int32_t target_id;
  float gain;
  uint8_t parameter;
} WavetableOscRoute_t;

static WavetableOscEntry_t *s_active_head[SAMPLE_VOICES];
static WavetableOscEntry_t *s_pending_head[SAMPLE_VOICES];
static WavetableOscEntry_t *s_pending_tail[SAMPLE_VOICES];
static WavetableOscEntry_t *s_active_render[SAMPLE_VOICES];
static WavetableOscEntry_t *s_pending_render[SAMPLE_VOICES];
static WavetableOscRoute_t *s_active_routes[SAMPLE_VOICES];
static WavetableOscRoute_t *s_pending_routes[SAMPLE_VOICES];
static float s_sample_frequency_mod[SAMPLE_VOICES];
static float s_sample_amplitude_gain[SAMPLE_VOICES];
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

static void WavetableOsc_ReleaseRoutes(WavetableOscRoute_t **head)
{
  WavetableOscRoute_t *route = *head;
  *head = NULL;
  while (route != NULL)
  {
    WavetableOscRoute_t *next = route->next;
    WavetableOsc_Deallocate(route);
    route = next;
  }
}

static WavetableOscEntry_t *WavetableOsc_Find(
    WavetableOscEntry_t *entry, uint32_t handle)
{
  while (entry != NULL)
  {
    if (entry->handle == handle) return entry;
    entry = entry->next;
  }
  return NULL;
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
  entry->render_next = NULL;
  entry->phase = 0u;
  entry->increment = (uint32_t)(increment + 0.5);
  entry->handle = handle;
  entry->frequency_hz = frequency_hz;
  entry->frequency_mod_hz = 0.0f;
  entry->amplitude_gain = 1.0f;
  entry->rendered_sample = 0;
  entry->length = (uint16_t)length;
  entry->wave_id = (uint8_t)wave_id;
  entry->explicit_route = 0u;
  entry->ordered = 0u;
  entry->dependency_count = 0u;

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

int WavetableOsc_AddRoutePending(uint8_t voice, uint32_t source_handle,
                                 int32_t target, uint8_t parameter,
                                 float gain)
{
  WavetableOscEntry_t *source;
  WavetableOscEntry_t *target_entry = NULL;
  WavetableOscRoute_t *route;

  if (voice >= SAMPLE_VOICES || !isfinite(gain) ||
      parameter >= FW_VM_CHANNEL_ROUTE_COUNT)
    return -1;
  source = WavetableOsc_Find(s_pending_head[voice], source_handle);
  if (source == NULL) return -1;
  if (target > 0)
  {
    target_entry = WavetableOsc_Find(s_pending_head[voice], (uint32_t)target);
    if (target_entry == NULL || target_entry == source) return -1;
  }
  if ((parameter == FW_VM_CHANNEL_ROUTE_AUDIO &&
       (target != FW_VM_CHANNEL_TARGET_OUTPUT || gain < 0.0f)) ||
      (parameter == FW_VM_CHANNEL_ROUTE_FREQUENCY &&
       target != FW_VM_CHANNEL_TARGET_SAMPLE && target_entry == NULL) ||
      (parameter == FW_VM_CHANNEL_ROUTE_AMPLITUDE &&
       (target != FW_VM_CHANNEL_TARGET_SAMPLE && target_entry == NULL)) ||
      (parameter == FW_VM_CHANNEL_ROUTE_AMPLITUDE &&
       (gain < 0.0f || gain > 1.0f)))
    return -1;

  for (route = s_pending_routes[voice]; route != NULL; route = route->next)
    if (route->source == source && route->target_id == target &&
        route->parameter == parameter)
      return -1;

  route = (WavetableOscRoute_t *)WavetableOsc_Allocate(sizeof(*route));
  if (route == NULL) return -1;
  route->source = source;
  route->target = target_entry;
  route->target_id = target;
  route->gain = gain;
  route->parameter = parameter;
  route->next = s_pending_routes[voice];
  s_pending_routes[voice] = route;
  source->explicit_route = 1u;
  s_pending_render[voice] = NULL;
  return 0;
}

int WavetableOsc_FinalizePending(uint8_t voice)
{
  WavetableOscEntry_t *entry;
  WavetableOscEntry_t *tail = NULL;
  WavetableOscRoute_t *route;
  uint32_t total = 0u;
  uint32_t ordered = 0u;

  if (voice >= SAMPLE_VOICES) return -1;
  s_pending_render[voice] = NULL;
  for (entry = s_pending_head[voice]; entry != NULL; entry = entry->next)
  {
    entry->render_next = NULL;
    entry->dependency_count = 0u;
    entry->ordered = 0u;
    total++;
  }
  for (route = s_pending_routes[voice]; route != NULL; route = route->next)
    if (route->target != NULL) route->target->dependency_count++;

  while (ordered < total)
  {
    WavetableOscEntry_t *selected = NULL;
    for (entry = s_pending_head[voice]; entry != NULL; entry = entry->next)
      if (entry->ordered == 0u && entry->dependency_count == 0u)
      {
        selected = entry;
        break;
      }
    if (selected == NULL)
    {
      s_pending_render[voice] = NULL;
      return -1;
    }
    selected->ordered = 1u;
    if (tail == NULL) s_pending_render[voice] = selected;
    else tail->render_next = selected;
    tail = selected;
    ordered++;
    for (route = s_pending_routes[voice]; route != NULL; route = route->next)
      if (route->source == selected && route->target != NULL)
        route->target->dependency_count--;
  }
  return 0;
}

void WavetableOsc_ActivatePending(uint8_t voice)
{
  WavetableOscEntry_t *entry;
  if (voice >= SAMPLE_VOICES)
  {
    return;
  }
  if (s_pending_head[voice] != NULL && s_pending_render[voice] == NULL &&
      WavetableOsc_FinalizePending(voice) != 0) return;
  WavetableOsc_StopActive(voice);
  s_active_head[voice] = s_pending_head[voice];
  s_active_routes[voice] = s_pending_routes[voice];
  s_active_render[voice] = s_pending_render[voice];
  s_pending_head[voice] = NULL;
  s_pending_tail[voice] = NULL;
  s_pending_routes[voice] = NULL;
  s_pending_render[voice] = NULL;
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
    WavetableOsc_ReleaseRoutes(&s_pending_routes[voice]);
    s_pending_tail[voice] = NULL;
    s_pending_render[voice] = NULL;
  }
}

void WavetableOsc_StopActive(uint8_t voice)
{
  if (voice < SAMPLE_VOICES)
  {
    WavetableOsc_ReleaseList(&s_active_head[voice]);
    WavetableOsc_ReleaseRoutes(&s_active_routes[voice]);
    s_active_render[voice] = NULL;
    s_sample_frequency_mod[voice] = 0.0f;
    s_sample_amplitude_gain[voice] = 1.0f;
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

static int32_t WavetableOsc_Saturate(double value)
{
  if (value > 2147483647.0) return INT32_MAX;
  if (value < -2147483648.0) return INT32_MIN;
  return (int32_t)value;
}

static int32_t WavetableOsc_Render(WavetableOscEntry_t *entry)
{
  double hz = (double)entry->frequency_hz +
              (double)entry->frequency_mod_hz;
  double increment;
  double amplitude;
  int32_t sample;
  if (!isfinite(hz)) hz = hz > 0.0 ? WAVETABLE_OSC_MAX_HZ : 0.0;
  if (hz < 0.0) hz = 0.0;
  if (hz > WAVETABLE_OSC_MAX_HZ) hz = WAVETABLE_OSC_MAX_HZ;
  increment = hz * (double)entry->length * (double)OSC_PHASE_ONE /
              (double)WAVETABLE_OSC_SAMPLE_RATE_HZ;
  entry->increment = (uint32_t)(increment + 0.5);
  sample = WavetableOsc_Next(entry);
  amplitude = (double)entry->amplitude_gain;
  if (!isfinite(amplitude)) amplitude = amplitude > 0.0 ? 2147483648.0 : 0.0;
  if (amplitude < 0.0) amplitude = 0.0;
  return WavetableOsc_Saturate((double)sample * amplitude);
}

void WavetableOsc_BeginSample(uint8_t voice, float *sample_frequency_hz,
                              float *sample_amplitude)
{
  WavetableOscEntry_t *entry;
  WavetableOscRoute_t *route;
  if (voice >= SAMPLE_VOICES)
  {
    if (sample_frequency_hz != NULL) *sample_frequency_hz = 0.0f;
    if (sample_amplitude != NULL) *sample_amplitude = 1.0f;
    return;
  }
  s_sample_frequency_mod[voice] = 0.0f;
  s_sample_amplitude_gain[voice] = 1.0f;
  for (entry = s_active_head[voice]; entry != NULL; entry = entry->next)
  {
    entry->frequency_mod_hz = 0.0f;
    entry->amplitude_gain = 1.0f;
  }
  for (entry = s_active_render[voice]; entry != NULL;
       entry = entry->render_next)
  {
    float normalized;
    entry->rendered_sample = WavetableOsc_Render(entry);
    normalized = (float)((double)entry->rendered_sample / 2147483648.0);
    for (route = s_active_routes[voice]; route != NULL; route = route->next)
    {
      if (route->source != entry) continue;
      if (route->parameter == FW_VM_CHANNEL_ROUTE_FREQUENCY)
      {
        if (route->target != NULL)
          route->target->frequency_mod_hz += normalized * route->gain;
        else
          s_sample_frequency_mod[voice] += normalized * route->gain;
      }
      else if (route->parameter == FW_VM_CHANNEL_ROUTE_AMPLITUDE)
      {
        float unipolar = normalized * 0.5f + 0.5f;
        float gain;
        if (unipolar < 0.0f) unipolar = 0.0f;
        if (unipolar > 1.0f) unipolar = 1.0f;
        gain = unipolar * route->gain;
        if (route->target != NULL)
          route->target->amplitude_gain *= gain;
        else
          s_sample_amplitude_gain[voice] *= gain;
      }
    }
  }
  if (sample_frequency_hz != NULL)
    *sample_frequency_hz = s_sample_frequency_mod[voice];
  if (sample_amplitude != NULL)
  {
    *sample_amplitude = s_sample_amplitude_gain[voice];
  }
}

int32_t WavetableOsc_MixSample(uint8_t voice, int32_t sample)
{
  WavetableOscEntry_t *entry;
  WavetableOscRoute_t *route;
  double weighted_sum;
  double total_weight = 1.0;
  float sample_amplitude;
  if (voice >= SAMPLE_VOICES) return sample;
  sample_amplitude = s_sample_amplitude_gain[voice];
  weighted_sum = (double)sample * (double)sample_amplitude;
  for (entry = s_active_head[voice]; entry != NULL; entry = entry->next)
  {
    double weight = entry->explicit_route == 0u ? 1.0 : 0.0;
    for (route = s_active_routes[voice]; route != NULL; route = route->next)
      if (route->source == entry &&
          route->parameter == FW_VM_CHANNEL_ROUTE_AUDIO)
        weight += route->gain;
    weighted_sum += (double)entry->rendered_sample * weight;
    total_weight += weight;
  }
  return WavetableOsc_Saturate(weighted_sum / total_weight);
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
