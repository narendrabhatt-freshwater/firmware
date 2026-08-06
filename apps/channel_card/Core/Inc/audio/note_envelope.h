/**
 ******************************************************************************
 * @file    note_envelope.h
 * @brief   Per-voice multi-segment amplitude envelope for the note bank.
 *
 * Chained linear ramps: each segment is (end_amp, slope). Start is always the
 * previous end (first note-on starts at 0). Last segment is release: end is
 * always 0; only slope is programmed. Gate: note-on runs pre-release then
 * holds; note-off releases from current amp. Pitch-track k vs C4 (see .c).
 * Unprogrammed voices bypass (env = 1). No heap.
 *
 * Console: en0 <end> <slope> [<end> <slope> ...] <release_slope>
 ******************************************************************************
 */

#ifndef __NOTE_ENVELOPE_H__
#define __NOTE_ENVELOPE_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

/** Must match NOTE_BANK_VOICES. */
#define NOTE_ENV_VOICES 16u

#define NOTE_ENV_SEGMENTS_MIN 2u
#define NOTE_ENV_SEGMENTS_MAX 10u

/** Max console floats: (MAX-1) end/slope pairs + final release slope. */
#define NOTE_ENV_CONSOLE_FLOATS_MAX \
  (((NOTE_ENV_SEGMENTS_MAX - 1u) * 2u) + 1u)

/** Pitch-track reference (middle C / C4), Hz. */
#define NOTE_ENV_F_REF_HZ 261.625565f

/** Console/API range for pitch-track constant k. */
#define NOTE_ENV_K_MIN 0.0f
#define NOTE_ENV_K_MAX 10.0f

  /**
   * One linear ramp target: end_amp 0..1, slope = |Δamp| per second (> 0).
   * Start is chained (previous end / current amp), not stored.
   * Release segment: end_amp is always 0.
   */
  typedef struct
  {
    float end_amp;
    float slope;
  } NoteEnv_Segment_t;

  /**
   * @brief Clear all voices to unprogrammed / idle (env bypass = 1.0).
   * @note Call once at bring-up before NoteBank_Init.
   */
  void NoteEnv_Init(void);

  /**
   * @brief Program segments for one voice (copies into static storage).
   * @param voice Voice 0..15.
   * @param segs  Segment array; last is release (end_amp forced to 0).
   * @param n     Count in [NOTE_ENV_SEGMENTS_MIN, NOTE_ENV_SEGMENTS_MAX].
   * @retval 0 Success.
   * @retval -1 voice OOR or NULL segs.
   * @retval -2 bad count / amps / slope.
   */
  int NoteEnv_SetSegments(uint8_t voice, const NoteEnv_Segment_t *segs,
                          uint8_t n);

  /** Clear program for voice → bypass (env=1, note-off hard-stops). */
  void NoteEnv_Clear(uint8_t voice);

  /** Non-zero if voice has a programmed envelope (n >= 2). */
  uint8_t NoteEnv_IsProgrammed(uint8_t voice);

  /** Segment count (0 if unprogrammed / invalid voice). */
  uint8_t NoteEnv_GetSegmentCount(uint8_t voice);

  /**
   * Copy one programmed segment. Returns 0 on success, -1 if voice/idx bad.
   */
  int NoteEnv_GetSegment(uint8_t voice, uint8_t idx, NoteEnv_Segment_t *out);

  /**
   * Pitch-track constant k (duration_scale via rate = (f/f_ref)^k).
   * Returns 0 on success, -1 voice OOR, -2 k out of [K_MIN, K_MAX].
   */
  int NoteEnv_SetPitchK(uint8_t voice, float k);

  /** Last k for voice (0 if invalid). */
  float NoteEnv_GetPitchK(uint8_t voice);

  /**
   * Note-on: restart at segment 0 from amp 0. No-op if unprogrammed.
   * freq_hz used to cache pitch_rate (cold path; may use powf).
   */
  void NoteEnv_NoteOn(uint8_t voice, float freq_hz);

  /**
   * Note-off: jump to release from current amp toward 0. No-op if idle.
   */
  void NoteEnv_NoteOff(uint8_t voice);

  /**
   * True while running, holding, or releasing (programmed voices only).
   * Unprogrammed → 0.
   */
  uint8_t NoteEnv_IsActive(uint8_t voice);

  /**
   * Next envelope amplitude 0..1 (hot path).
   * Unprogrammed → 1.0f. Idle programmed → 0.0f.
   */
  float NoteEnv_Process(uint8_t voice);

#ifdef __cplusplus
}
#endif

#endif /* __NOTE_ENVELOPE_H__ */
