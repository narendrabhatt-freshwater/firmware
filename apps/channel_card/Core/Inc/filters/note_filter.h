/**
 ******************************************************************************
 * @file    note_filter.h
 * @brief   Per-voice 4-pole Butterworth LPF for the note bank.
 *
 * Voice-bank wrapper around butterworth_four_pole. Hot path uses double
 * (H725 FPU) intentionally; Q31 only at the NoteFilter_Process boundary.
 * HP/BP deferred — pass API is LP-only.
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

/** Audible cutoff range (Hz). Max also means bypass (transparent).
 *  Console/API also accept 0.0 as an alias for max (bypass). */
#define NOTE_FILTER_CUTOFF_MIN_HZ 20.0
#define NOTE_FILTER_CUTOFF_MAX_HZ 20000.0

/** Pass type. v1: LP only (HP/BP reserved for later). */
typedef enum {
  NOTE_FILTER_PASS_LP = 0,
  NOTE_FILTER_PASS_HP = 1,
  NOTE_FILTER_PASS_BP = 2
} NoteFilter_Pass_t;

/**
 * Set one voice's LPF cutoff (Hz). voice: 0..15.
 * Returns 0 on success, -1 if voice out of range, -2 if cutoff out of range.
 * At NOTE_FILTER_CUTOFF_MAX_HZ (or 0.0 alias) the filter is bypassed (identity).
 * Clears delay state on every successful set.
 */
int NoteFilter_SetCutoff(uint8_t voice, double cutoff_hz);

/** Last cutoff set for voice (Hz). MAX if never configured / invalid. */
double NoteFilter_GetCutoff(uint8_t voice);

/**
 * Set pass type. v1 accepts NOTE_FILTER_PASS_LP only; HP/BP return -2.
 * Redesigns from current cutoff unless bypassed.
 */
int NoteFilter_SetPass(uint8_t voice, NoteFilter_Pass_t pass);

/** Current pass type for voice (always LP in v1 if valid). */
NoteFilter_Pass_t NoteFilter_GetPass(uint8_t voice);

/** Stable name: "lp" (v1); "?" if unknown. */
const char *NoteFilter_PassName(NoteFilter_Pass_t pass);

/** Zero delay lines for one voice (call on note-off / steal). */
void NoteFilter_Reset(uint8_t voice);

/**
 * Process one Q31 sample through the voice's 4-pole LPF (float internally).
 * Bypass is a no-op return of x.
 */
int32_t NoteFilter_Process(uint8_t voice, int32_t x);

#ifdef __cplusplus
}
#endif

#endif /* __NOTE_FILTER_H__ */
