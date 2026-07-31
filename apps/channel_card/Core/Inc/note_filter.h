/**
 ******************************************************************************
 * @file    note_filter.h
 * @brief   Per-voice 4-pole Butterworth LPF (two cascaded biquads) for the
 *          note bank. Hot path is fixed-point Q31 I/O; coeff design uses
 *          double on the cold path only.
 ******************************************************************************
 */

#ifndef __NOTE_FILTER_H__
#define __NOTE_FILTER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** Must match NOTE_BANK_VOICES. */
#define NOTE_FILTER_VOICES 16u

/** Audible cutoff range (Hz). Max also means bypass (transparent). */
#define NOTE_FILTER_CUTOFF_MIN_HZ 20.0
#define NOTE_FILTER_CUTOFF_MAX_HZ 20000.0

/**
 * Set one voice's LPF cutoff. voice: 0..15.
 * Returns 0 on success, -1 if voice out of range, -2 if cutoff out of range.
 * At NOTE_FILTER_CUTOFF_MAX_HZ the filter is bypassed (identity).
 * Clears delay state on every successful set (avoids stale energy after jumps).
 */
int NoteFilter_SetCutoff(uint8_t voice, double cutoff_hz);

/** Last cutoff set for voice (Hz). 0 if voice invalid. */
double NoteFilter_GetCutoff(uint8_t voice);

/** Zero delay lines for one voice (call on note-off / steal). */
void NoteFilter_Reset(uint8_t voice);

/**
 * Process one Q31 sample through the voice's 4-pole LPF.
 * Hot path — integer only. Bypass is a no-op return of x.
 */
int32_t NoteFilter_Process(uint8_t voice, int32_t x);

#ifdef __cplusplus
}
#endif

#endif /* __NOTE_FILTER_H__ */
