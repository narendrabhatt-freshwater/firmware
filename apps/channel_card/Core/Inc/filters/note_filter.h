/**
 ******************************************************************************
 * @file    note_filter.h
 * @brief   Per-voice 4-pole Butterworth LPF for the note bank.
 *
 * Voice-bank wrapper around butterworth_four_pole. The hot path uses double
 * (H725 FPU) intentionally; Q31 conversion happens only at
 * NoteFilter_Process edges. Pass API is low-pass only in this build;
 * high-pass / band-pass enum values are reserved.
 ******************************************************************************
 */

#ifndef __NOTE_FILTER_H__
#define __NOTE_FILTER_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

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

  /** Pass type. This build accepts NOTE_FILTER_PASS_LP only. */
  typedef enum
  {
    NOTE_FILTER_PASS_LP = 0,
    NOTE_FILTER_PASS_HP = 1,
    NOTE_FILTER_PASS_BP = 2
  } NoteFilter_Pass_t;

  /**
   * @brief Establish default state for all voices (LP, default q, bypass).
   * @note Call once at bring-up before note-bank playback. Safe to call again.
   */
  void NoteFilter_InitAll(void);

  /**
   * @brief Set one voice's LPF cutoff (Hz).
   * @param voice      Voice index 0..15.
   * @param cutoff_hz  Cutoff in Hz; 0.0 aliases to NOTE_FILTER_CUTOFF_MAX_HZ.
   * @retval 0 Success.
   * @retval -1 voice out of range.
   * @retval -2 cutoff out of range (after 0→max alias).
   * @note At MAX (or 0 alias) the filter is bypassed (identity) and delays
   *       are cleared. Otherwise redesigns with the voice's current q and
   *       clears delay state.
   */
  int NoteFilter_SetCutoff(uint8_t voice, double cutoff_hz);

  /**
   * @brief Last cutoff set for voice (Hz).
   * @param voice Voice index 0..15.
   * @return Cutoff Hz, or NOTE_FILTER_CUTOFF_MAX_HZ if never configured /
   *         voice invalid.
   */
  double NoteFilter_GetCutoff(uint8_t voice);

  /**
   * @brief Set DF4 g/q for voice.
   * @param voice Voice index 0..15.
   * @param q     Shape in [NOTE_FILTER_Q_MIN, NOTE_FILTER_Q_MAX].
   * @retval 0 Success.
   * @retval -1 voice out of range.
   * @retval -2 q out of range.
   * @note If not bypassed, redesigns from current cutoff (clears delays).
   */
  int NoteFilter_SetQ(uint8_t voice, double q);

  /**
   * @brief Last q set for voice.
   * @param voice Voice index 0..15.
   * @return Stored q, or NOTE_FILTER_Q_DEFAULT if never configured / invalid.
   */
  double NoteFilter_GetQ(uint8_t voice);

  /**
   * @brief Set pass type for voice.
   * @param voice Voice index 0..15.
   * @param pass  Pass enum; only NOTE_FILTER_PASS_LP is accepted.
   * @retval 0 Success.
   * @retval -1 voice out of range.
   * @retval -2 unsupported pass (HP/BP).
   * @note Redesigns from current cutoff unless bypassed.
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

#endif /* __NOTE_FILTER_H__ */
