/*
                                   __
                               ___  \  \
                          ___  \  \  \  \    _______
                     ___  \  \  \  \  \__\__/_____  \
                     \  \  \  \  \__\_______      \__\____
                      \  \  \  \     ___ \  \________  \__/
                       \  \  \__\___/_  \ \___/   \  \___
                        \  \    ____  \  \________ \ ___/
                         \__\__/ \  \  \____/  \  \
                                  \  \     ___  \  \___
                                   \__\___/ \  \ \____/
                                           \  \
                                              \  \___
                 __                 _          \ ___/  _
                / _|_ __  ___  ___ | |____      ____ _| |_  ___  _ __
               | |_| '_ |/ _ \/ __|| '_ \ \ /\ / / _` | __|/ _ \| '__|
               |  _| |  |  __/\__ \| | | \ V  V / (_| | |_|  __/| |
               |_| |_|   \___||___/|_| |_|\_/\_/ \__,_|\__|\___||_|

            (C) 2 0 2 6   F r e s h w a t e r   I n s t r u m e n t s
*/

#ifndef CHANNEL_APP_OSCILLATOR_H
#define CHANNEL_APP_OSCILLATOR_H

#include <stdint.h>
#include "audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- interpolate sample tables ------------------------------------------ */

/* 8-tap Hann-windowed sinc (Q16.16 index → Q31).
 * Linear interpolation is 2-tap. This kernel is 8-tap. Coeffs are built
 * once at init (float OK); the sample path is integer only. */

#define INTERP8_TAPS 8u
#define INTERP8_PHASES 256u

void init_sample_interpolation(void);

/**
 * @brief Read int16 table at Q16.16 phase.
 * @param wrap  non-zero: modulo n (sustain loop). zero: clamp ends.
 */
int32_t interpolate_int16_sample(const int16_t *tab, uint32_t n, uint32_t phase_q16, uint8_t wrap);

/** Same for a Q31 table (attack heads). */
int32_t interpolate_q31_sample(const int32_t *tab, uint32_t n, uint32_t phase_q16, uint8_t wrap);

/**
 * @brief 8-tap kernel over prefetched Q31 samples.
 *        taps[t] is source at (i0 + t - 3); phase_q16 supplies the
 *        fractional LUT index only.
 */
int32_t interpolate_q31_taps(const int32_t taps[INTERP8_TAPS], uint32_t phase_q16);

/**
 * @brief 8-tap kernel over prefetched int16 samples (ring body path).
 *        taps[t] is source at (i0 + t - 3); phase_q16 supplies the
 *        fractional LUT index only.
 */
int32_t interpolate_int16_taps(const int16_t taps[INTERP8_TAPS], uint32_t phase_q16);

/* ---- configure wavetable oscillators ------------------------------------ */

/* Automatically allocated attack-bank wavetable oscillators. */

#define WAVETABLE_OSC_WAVE_COUNT ATTACK_BANK_WAVETABLE_COUNT
#define WAVETABLE_OSC_WAVE_FIRST ATTACK_BANK_WAVETABLE_FIRST
#define WAVETABLE_OSC_SAMPLE_RATE_HZ AUDIO_SAMPLE_RATE_HZ
#define WAVETABLE_OSC_MAX_HZ 24000.0f

void init_wavetable_oscillators(void);
void begin_pending_oscillators(uint8_t voice);

/** Append an oscillator and return a positive, opaque note-local handle. */
int add_pending_oscillator(uint8_t voice, uint8_t wave, float frequency_hz, uint32_t *handle_out);
int add_pending_oscillator_route(uint8_t voice, uint32_t source_handle, int32_t target,
                                 uint8_t parameter, float gain);
/** Validate the pending graph and establish its deterministic render order. */
int finalize_pending_oscillators(uint8_t voice);

void activate_pending_oscillators(uint8_t voice);
void discard_pending_oscillators(uint8_t voice);
void stop_active_oscillators(uint8_t voice);
void stop_voice_oscillators(uint8_t voice);
void stop_all_oscillators(void);

uint8_t is_oscillator_handle_valid(uint8_t voice, uint32_t handle);
int64_t mix_next_oscillator_samples(uint8_t voice, uint32_t *count_out);

/** Render all oscillator nodes once and return modulation for SAMPLE. */
void apply_oscillator_sample_modulation(uint8_t voice, float *sample_frequency_hz,
                                        float *sample_amplitude);
/** Apply SAMPLE amplitude and combine it with oscillator OUTPUT routes. */
int32_t mix_voice_oscillators(uint8_t voice, int32_t sample);

#ifdef __cplusplus
}
#endif

#endif
