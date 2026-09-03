/**
 ******************************************************************************
 * @file    wavetable_osc.h
 * @brief   Automatically allocated attack-bank wavetable oscillators.
 ******************************************************************************
 */

#ifndef __WAVETABLE_OSC_H__
#define __WAVETABLE_OSC_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

#include "attack_bank.h"

#define WAVETABLE_OSC_WAVE_COUNT ATTACK_BANK_WAVETABLE_COUNT
#define WAVETABLE_OSC_WAVE_FIRST ATTACK_BANK_WAVETABLE_FIRST
#define WAVETABLE_OSC_SAMPLE_RATE_HZ 48000u
#define WAVETABLE_OSC_MAX_HZ 24000.0f

  void WavetableOsc_Init(void);
  void WavetableOsc_BeginPending(uint8_t voice);

  /** Append an oscillator and return a positive, opaque note-local handle. */
  int WavetableOsc_AddPending(uint8_t voice, uint8_t wave,
                              float frequency_hz, uint32_t *handle_out);
  int WavetableOsc_AddRoutePending(uint8_t voice, uint32_t source_handle,
                                   int32_t target, uint8_t parameter,
                                   float gain);
  /** Validate the pending graph and establish its deterministic render order. */
  int WavetableOsc_FinalizePending(uint8_t voice);

  void WavetableOsc_ActivatePending(uint8_t voice);
  void WavetableOsc_DiscardPending(uint8_t voice);
  void WavetableOsc_StopActive(uint8_t voice);
  void WavetableOsc_Stop(uint8_t voice);
  void WavetableOsc_StopAll(void);

  uint8_t WavetableOsc_HandleIsValid(uint8_t voice, uint32_t handle);
  int64_t WavetableOsc_NextSum(uint8_t voice, uint32_t *count_out);

  /** Render all oscillator nodes once and return modulation for SAMPLE. */
  void WavetableOsc_BeginSample(uint8_t voice, float *sample_frequency_hz,
                                float *sample_amplitude);
  /** Apply SAMPLE amplitude and combine it with oscillator OUTPUT routes. */
  int32_t WavetableOsc_MixSample(uint8_t voice, int32_t sample);

#ifdef __cplusplus
}
#endif

#endif /* __WAVETABLE_OSC_H__ */
