#include "wavetable_osc.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int8_t tables[ATTACK_BANK_COUNT][ATTACK_BANK_LEN];
static uint32_t lengths[ATTACK_BANK_COUNT];

#define STRESS_OSCILLATOR_INSTANCES 512u

static size_t successful_allocations;
static size_t live_allocations;
static size_t fail_after = SIZE_MAX;

static void Check(int condition, const char *message);

void *WavetableOsc_Allocate(size_t size)
{
  void *pointer;
  if (successful_allocations >= fail_after)
  {
    return NULL;
  }
  pointer = malloc(size);
  if (pointer != NULL)
  {
    successful_allocations++;
    live_allocations++;
  }
  return pointer;
}

void WavetableOsc_Deallocate(void *pointer)
{
  if (pointer != NULL)
  {
    Check(live_allocations > 0u, "allocator live count must not underflow");
    live_allocations--;
    free(pointer);
  }
}

uint32_t AttackBank_GetLen(uint16_t wave_id)
{
  return wave_id < ATTACK_BANK_COUNT ? lengths[wave_id] : 0u;
}

const int8_t *AttackBank_Table(uint16_t wave_id)
{
  return wave_id < ATTACK_BANK_COUNT ? tables[wave_id] : NULL;
}

static void Check(int condition, const char *message)
{
  if (!condition)
  {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

int main(void)
{
  uint32_t handle0;
  uint32_t handle1;
  uint32_t active0;
  uint32_t replacement;
  uint32_t handles[STRESS_OSCILLATOR_INSTANCES];
  uint32_t count;
  int64_t sample;
  uint16_t i;

  lengths[248] = 4u;
  tables[248][0] = 0;
  tables[248][1] = 64;
  tables[248][2] = 0;
  tables[248][3] = -64;
  lengths[255] = 2u;
  tables[255][0] = 32;
  tables[255][1] = -32;
  lengths[249] = 1u;
  lengths[250] = ATTACK_BANK_LEN;
  for (i = 0u; i < ATTACK_BANK_LEN; i++)
  {
    tables[250][i] = 5;
  }

  WavetableOsc_Init();
  Check(WavetableOsc_NextSum(0u, &count) == 0 && count == 0u,
        "reset must be silent");
  Check(WavetableOsc_AddPending(0u, 0u, 12000.0f, &handle0) == 0 &&
            handle0 > 0u && handle0 <= UINT32_C(0x00FFFFFF),
        "logical wave zero must map to attack ID 248");
  active0 = handle0;
  Check(WavetableOsc_HandleIsValid(0u, handle0) != 0u &&
            WavetableOsc_HandleIsValid(1u, handle0) == 0u,
        "handle must be valid only for its owning voice");
  Check(WavetableOsc_NextSum(0u, &count) == 0 && count == 0u,
        "pending oscillator must not alter active sound");
  WavetableOsc_ActivatePending(0u);
  Check(WavetableOsc_HandleIsValid(0u, handle0) != 0u,
        "handle must survive pending promotion");
  Check(WavetableOsc_NextSum(0u, &count) == 0 && count == 1u,
        "phase must start at table zero");
  Check(WavetableOsc_NextSum(0u, &count) == 64LL * 16777216LL,
        "frequency must advance one table sample");
  Check(WavetableOsc_NextSum(0u, &count) == 0,
        "third wavetable point must render");
  Check(WavetableOsc_NextSum(0u, &count) == -64LL * 16777216LL,
        "fourth wavetable point must render");
  Check(WavetableOsc_NextSum(0u, &count) == 0,
        "last sample must wrap cyclically to the first");

  WavetableOsc_BeginPending(1u);
  Check(WavetableOsc_AddPending(1u, 7u, 12000.0f, &handle0) == 0,
        "logical wave seven must map to attack ID 255");
  Check(WavetableOsc_AddPending(1u, 7u, 6000.0f, &handle1) == 0 &&
            handle0 != handle1,
        "the same waveform must create independent pitched oscillators");
  WavetableOsc_ActivatePending(1u);
  sample = WavetableOsc_NextSum(1u, &count);
  Check(count == 2u && sample == 64LL * 16777216LL,
        "automatically allocated oscillators must sum independently");
  sample = WavetableOsc_NextSum(1u, &count);
  Check(count == 2u && sample == 16LL * 16777216LL,
        "duplicate waveforms must advance at independent pitches");

  WavetableOsc_BeginPending(2u);
  Check(WavetableOsc_AddPending(2u, 2u, 440.0f, &replacement) == 0,
        "a 512-sample wavetable must be usable");
  WavetableOsc_ActivatePending(2u);
  Check(WavetableOsc_NextSum(2u, &count) == 5LL * 16777216LL && count == 1u,
        "arbitrary wavetable lengths through 512 must render");
  WavetableOsc_Stop(2u);

  WavetableOsc_BeginPending(0u);
  Check(WavetableOsc_AddPending(0u, 7u, 6000.0f, &replacement) == 0,
        "replacement oscillator must configure independently");
  WavetableOsc_DiscardPending(0u);
  Check(WavetableOsc_HandleIsValid(0u, replacement) == 0u &&
            WavetableOsc_HandleIsValid(0u, handle0) == 0u,
        "discard must invalidate pending and foreign handles");
  Check(WavetableOsc_HandleIsValid(0u, active0) != 0u,
        "discarding a replacement must preserve the active handle");
  Check(WavetableOsc_NextSum(0u, &count) == 64LL * 16777216LL &&
            count == 1u,
        "discarding a replacement must preserve the active oscillator");

  Check(WavetableOsc_AddPending(0u, 7u, 6000.0f, &replacement) == 0,
        "a new replacement must allocate after discard");
  WavetableOsc_ActivatePending(0u);
  Check(WavetableOsc_HandleIsValid(0u, active0) == 0u &&
            WavetableOsc_HandleIsValid(0u, replacement) != 0u,
        "promotion must retire the old note and preserve the new handle");
  Check(WavetableOsc_NextSum(0u, &count) == 32LL * 16777216LL && count == 1u,
        "promotion must reset the replacement phase to zero");

  Check(WavetableOsc_AddPending(0u, 8u, 440.0f, &replacement) != 0,
        "logical wave eight must fail");
  Check(WavetableOsc_AddPending(0u, 1u, 440.0f, &replacement) != 0,
        "one-sample wavetable must fail");
  Check(WavetableOsc_AddPending(0u, 3u, 440.0f, &replacement) != 0,
        "unloaded wavetable must fail");
  Check(WavetableOsc_AddPending(0u, 0u, 0.0f, &replacement) != 0,
        "zero frequency must fail");
  Check(WavetableOsc_AddPending(0u, 0u, 24000.1f, &replacement) != 0,
        "frequency above Nyquist must fail");

  WavetableOsc_Init();
  for (i = 0u; i < STRESS_OSCILLATOR_INSTANCES; i++)
  {
    Check(WavetableOsc_AddPending(0u, 0u, 440.0f, &handles[i]) == 0,
          "dynamic allocation must create 512 same-table oscillators");
  }
  Check(WavetableOsc_AddPending(1u, 0u, 440.0f, &replacement) == 0,
        "a 513th oscillator must prove 512 is not a capacity boundary");
  fail_after = successful_allocations;
  Check(WavetableOsc_AddPending(1u, 0u, 440.0f, &handle1) != 0,
        "runtime allocator exhaustion must fail deterministically");
  fail_after = SIZE_MAX;
  handle0 = handles[STRESS_OSCILLATOR_INSTANCES - 1u];
  WavetableOsc_DiscardPending(0u);
  Check(WavetableOsc_HandleIsValid(0u, handle0) == 0u,
        "released handle must become stale");
  Check(WavetableOsc_AddPending(1u, 0u, 440.0f, &handle1) == 0 &&
            handle1 != handle0 &&
            WavetableOsc_HandleIsValid(1u, handle1) != 0u,
        "allocation after release must receive a new opaque handle");
  WavetableOsc_StopAll();
  Check(WavetableOsc_HandleIsValid(1u, handle1) == 0u &&
            WavetableOsc_NextSum(1u, &count) == 0 && count == 0u,
        "stop-all must release every oscillator");
  Check(live_allocations == 0u,
        "stop-all must return every dynamic descriptor to the allocator");

  puts("Dynamic wavetable oscillator test passed");
  return 0;
}
