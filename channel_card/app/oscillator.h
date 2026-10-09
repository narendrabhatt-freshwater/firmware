#ifndef CHANNEL_APP_OSCILLATOR_H
#define CHANNEL_APP_OSCILLATOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- interpolate sample tables ------------------------------------------ */

/* 8-tap Hann-windowed sinc (Q16.16 index → Q31).
 * Linear interpolation is 2-tap. This kernel is 8-tap. Coeffs are built
 * once at init (float OK); the sample path is integer only. */

#define INTERP8_TAPS 8u
#define INTERP8_PHASES 256u

void Interp8_Init(void);

/**
 * @brief Read int16 table at Q16.16 phase.
 * @param wrap  non-zero: modulo n (sustain loop). zero: clamp ends.
 */
int32_t Interp8_S16(const int16_t *tab, uint32_t n, uint32_t phase_q16, uint8_t wrap);

/** Same for a Q31 table (attack heads). */
int32_t Interp8_Q31(const int32_t *tab, uint32_t n, uint32_t phase_q16, uint8_t wrap);

/**
 * @brief 8-tap kernel over prefetched Q31 samples.
 *        taps[t] is source at (i0 + t - 3); phase_q16 supplies the
 *        fractional LUT index only.
 */
int32_t Interp8_Q31Taps(const int32_t taps[INTERP8_TAPS], uint32_t phase_q16);

/**
 * @brief 8-tap kernel over prefetched int16 samples (ring body path).
 *        taps[t] is source at (i0 + t - 3); phase_q16 supplies the
 *        fractional LUT index only.
 */
int32_t Interp8_S16Taps(const int16_t taps[INTERP8_TAPS], uint32_t phase_q16);

/* ---- configure wavetable oscillators ------------------------------------ */

/* Automatically allocated attack-bank wavetable oscillators. */

#define WAVETABLE_OSC_WAVE_COUNT ATTACK_BANK_WAVETABLE_COUNT
#define WAVETABLE_OSC_WAVE_FIRST ATTACK_BANK_WAVETABLE_FIRST
#define WAVETABLE_OSC_SAMPLE_RATE_HZ 48000u
#define WAVETABLE_OSC_MAX_HZ 24000.0f

void WavetableOsc_Init(void);
void WavetableOsc_BeginPending(uint8_t voice);

/** Append an oscillator and return a positive, opaque note-local handle. */
int WavetableOsc_AddPending(uint8_t voice, uint8_t wave, float frequency_hz, uint32_t *handle_out);
int WavetableOsc_AddRoutePending(uint8_t voice, uint32_t source_handle, int32_t target,
                                 uint8_t parameter, float gain);
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
void WavetableOsc_BeginSample(uint8_t voice, float *sample_frequency_hz, float *sample_amplitude);
/** Apply SAMPLE amplitude and combine it with oscillator OUTPUT routes. */
int32_t WavetableOsc_MixSample(uint8_t voice, int32_t sample);

#ifdef __cplusplus
}
#endif

#endif
