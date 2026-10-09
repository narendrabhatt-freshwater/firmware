#ifndef CHANNEL_APP_FILTER_H
#define CHANNEL_APP_FILTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- design four-pole filters ------------------------------------------- */

/* Reusable 4-pole DF4 Butterworth filter kernel.
 * Direct-form algorithm ported from the reference four_pole_filter
 * implementation (init_low_pass / init_high_pass / filter /
 * cutoff_to_omega). Coefficients and delays are double; callers own
 * sample-rate selection and any fixed-point conversion at the edges.
 * Parameter g (1.0 ≈ classical Butterworth) shapes peaking near fc:
 * higher g → more peak. Not RBJ biquad Q and not analog VCF resonance. */

/**
 * @brief Filter state: nine feedforward/feedback coefficients and four delays.
 */
typedef struct {
    double coef[9];
    double d[4];
} ButterFourPole_t;

/**
 * @brief Convert a cutoff in Hz to digital omega = 2π·f/fs.
 * @param cutoff_hz   Desired cutoff (Hz). Clamped to [0, 0.999·Nyquist].
 * @param sample_rate Sample rate (Hz). Must match AUDIO_SAMPLE_RATE_HZ in use.
 * @return omega in radians/sample.
 */
double ButterFourPole_CutoffToOmega(double cutoff_hz, double sample_rate);

/**
 * @brief Design a low-pass section at omega with shape parameter g.
 * @param f     Filter instance (non-NULL).
 * @param omega Digital cutoff from ButterFourPole_CutoffToOmega().
 * @param g     Shape (typically 0.5..10; 1.0 ≈ Butterworth).
 * @note Clears delay lines after loading coefficients.
 */
void ButterFourPole_InitLowPass(ButterFourPole_t *f, double omega, double g);

/**
 * @brief Design a high-pass section at omega with shape parameter g.
 * @param f     Filter instance (non-NULL).
 * @param omega Digital cutoff from ButterFourPole_CutoffToOmega().
 * @param g     Shape (typically 0.5..10; 1.0 ≈ Butterworth).
 * @note Clears delay lines after loading coefficients.
 */
void ButterFourPole_InitHighPass(ButterFourPole_t *f, double omega, double g);

/**
 * @brief Zero delay lines only; coefficients are left unchanged.
 * @param f Filter instance (non-NULL).
 */
void ButterFourPole_Reset(ButterFourPole_t *f);

/**
 * @brief Process one sample through the DF4 section.
 * @param f  Filter instance (non-NULL).
 * @param in Input sample (same units as the design, typically ±1.0).
 * @return Filtered output sample.
 */
double ButterFourPole_Process(ButterFourPole_t *f, double in);

/* ---- configure voice filters -------------------------------------------- */

/* Per-voice 4-pole Butterworth LPF for the note bank.
 * Voice-bank wrapper around butterworth_four_pole. The hot path uses double
 * (H725 FPU) intentionally; Q31 conversion happens only at
 * NoteFilter_Process edges. Pass API is low-pass only in this build;
 * high-pass / band-pass enum values are reserved.
 * Pitch-track (CMI / classic key follow): f0 sets fbase at C4; fk sets k;
 * fc = fbase * (f_note/C4)^k = fbase * 2^(k*n/12). Cold-path redesign only. */

/** Must match NOTE_BANK_VOICES. */
#define NOTE_FILTER_VOICES 16u

/**
 * Audible cutoff range (Hz). Max also means bypass (transparent).
 * Console/API also accept 0.0 as an alias for max (bypass).
 */
#define NOTE_FILTER_CUTOFF_MIN_HZ 20.0
#define NOTE_FILTER_CUTOFF_MAX_HZ 20000.0

/**
 * DF4 shape parameter g (console labels it q). 1.0 ≈ Butterworth.
 * Higher → more peaking near fc. Not RBJ biquad Q / analog VCF resonance.
 */
#define NOTE_FILTER_Q_MIN 0.5
#define NOTE_FILTER_Q_MAX 10.0
#define NOTE_FILTER_Q_DEFAULT 1.0

/**
 * Pitch-track reference (middle C / C4), Hz.
 * Middle-C reference used for filter pitch tracking.
 */
#define NOTE_FILTER_F_REF_HZ 261.625565

/** Pitch-track k: 0 = absolute (fc = fbase); 1 = full 1:1 key follow. */
#define NOTE_FILTER_K_MIN 0.0
#define NOTE_FILTER_K_MAX 10.0
#define NOTE_FILTER_K_DEFAULT 0.0

/** Pass type. This build accepts NOTE_FILTER_PASS_LP only. */
typedef enum {
    NOTE_FILTER_PASS_LP = 0,
    NOTE_FILTER_PASS_HP = 1,
    NOTE_FILTER_PASS_BP = 2
} NoteFilter_Pass_t;

/**
 * @brief Establish default state for all voices (LP, default q/k, bypass).
 * @note Call once at bring-up before note-bank playback. Safe to call again.
 */
void NoteFilter_InitAll(void);

/**
 * @brief Set one voice's base LPF cutoff at C4 (Hz).
 * @param voice      Voice index 0..15.
 * @param cutoff_hz  Base cutoff Hz; 0.0 aliases to NOTE_FILTER_CUTOFF_MAX_HZ.
 * @retval 0 Success.
 * @retval -1 voice out of range.
 * @retval -2 cutoff out of range (after 0→max alias), or q invalid.
 * @note At MAX (or 0 alias) the filter is bypassed (identity) and delays
 *       are cleared. Otherwise applies pitch-track (if k>0) and redesigns.
 */
int NoteFilter_SetCutoff(uint8_t voice, double cutoff_hz);

/**
 * @brief Effective cutoff last designed for voice (Hz).
 * @param voice Voice index 0..15.
 * @return Effective fc, or NOTE_FILTER_CUTOFF_MAX_HZ if bypassed / invalid.
 */
double NoteFilter_GetCutoff(uint8_t voice);

/**
 * @brief Programmed base cutoff at C4 (Hz), before pitch-track.
 * @param voice Voice index 0..15.
 * @return Base Hz, or NOTE_FILTER_CUTOFF_MAX_HZ if never set / invalid.
 */
double NoteFilter_GetBaseCutoff(uint8_t voice);

/**
 * @brief Set DF4 g/q for voice.
 * @param voice Voice index 0..15.
 * @param q     Shape in [NOTE_FILTER_Q_MIN, NOTE_FILTER_Q_MAX].
 * @retval 0 Success.
 * @retval -1 voice out of range.
 * @retval -2 q out of range.
 * @note If not bypassed, redesigns from current effective cutoff.
 */
int NoteFilter_SetQ(uint8_t voice, double q);

/**
 * @brief Last q set for voice.
 * @param voice Voice index 0..15.
 * @return Stored q, or NOTE_FILTER_Q_DEFAULT if never configured / invalid.
 */
double NoteFilter_GetQ(uint8_t voice);

/**
 * @brief Set pitch-track constant k (fc = fbase * (f_note/C4)^k).
 * @param voice Voice index 0..15.
 * @param k     Track amount in [NOTE_FILTER_K_MIN, NOTE_FILTER_K_MAX].
 * @retval 0 Success.
 * @retval -1 voice out of range.
 * @retval -2 k out of range.
 * @note Reapplies effective cutoff from cached note freq when not bypassed.
 */
int NoteFilter_SetPitchK(uint8_t voice, double k);

/**
 * @brief Last pitch-track k for voice.
 * @param voice Voice index 0..15.
 * @return Stored k, or NOTE_FILTER_K_DEFAULT if invalid.
 */
double NoteFilter_GetPitchK(uint8_t voice);

/**
 * @brief Note-on / pitch change: cache freq and retune if k > 0.
 * @param voice   Voice index 0..15.
 * @param freq_hz Oscillator frequency (Hz). <=0 is ignored (no redesign).
 * @note Cold path only. No-op when bypassed.
 */
void NoteFilter_OnNoteFreq(uint8_t voice, double freq_hz);

/**
 * @brief Set pass type for voice.
 * @param voice Voice index 0..15.
 * @param pass  Pass enum; only NOTE_FILTER_PASS_LP is accepted.
 * @retval 0 Success.
 * @retval -1 voice out of range.
 * @retval -2 unsupported pass (HP/BP).
 * @note Redesigns from current base unless bypassed.
 */
int NoteFilter_SetPass(uint8_t voice, NoteFilter_Pass_t pass);

/**
 * @brief Current pass type for voice (always LP in this build if voice valid).
 * @param voice Voice index 0..15 (ignored while only LP is implemented).
 * @return NOTE_FILTER_PASS_LP.
 */
NoteFilter_Pass_t NoteFilter_GetPass(uint8_t voice);

/**
 * @brief Stable short name for a pass enum.
 * @param pass Pass type.
 * @return "lp", "hp", "bp", or "?" if unknown.
 */
const char *NoteFilter_PassName(NoteFilter_Pass_t pass);

/**
 * @brief Zero delay lines for one voice (call on note-off / steal).
 * @param voice Voice index 0..15; out-of-range is a no-op.
 */
void NoteFilter_Reset(uint8_t voice);

/**
 * @brief Process one Q31 sample through the voice's 4-pole LPF.
 * @param voice Voice index 0..15.
 * @param x     Input sample (Q31).
 * @return Filtered Q31 sample, or x unchanged when bypassed / invalid voice.
 */
int32_t NoteFilter_Process(uint8_t voice, int32_t x);

#ifdef __cplusplus
}
#endif

#endif
