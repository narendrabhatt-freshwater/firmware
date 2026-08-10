/**
 ******************************************************************************
 * @file    note_bank.h
 * @brief   SAMPLE voice bank: attack head → dry UAC ring → env → filter → mix.
 *
 * Voices n0..n7 (SAMPLE_VOICES). Each note-on plays the assigned attack
 * table then continues from the per-voice dry stream ring. Envelope and
 * LPF run for the full voice lifetime on the card.
 ******************************************************************************
 */

#ifndef __NOTE_BANK_H__
#define __NOTE_BANK_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

#include "attack_bank.h"

/** Voices available in SAMPLE mode (n0..n7). */
#define NOTE_BANK_VOICES SAMPLE_VOICES

  /** Kept for console compatibility; SAMPLE ignores oscillator shape. */
  typedef enum
  {
    NOTE_SHAPE_SINE = 0,
    NOTE_SHAPE_PULSE = 1,
    NOTE_SHAPE_TRI = 2
  } NoteBank_Shape_t;

  void NoteBank_Init(void);
  void NoteBank_PanicAll(void);

  /**
   * @brief Note on/off. freq_hz <= 0 releases/stops; >0 starts attack+stream.
   * Uses wave_id assigned via NoteBank_SetWaveId (default = voice index).
   */
  void NoteBank_SetFreq(uint8_t note, double freq_hz, double scale);

  double NoteBank_GetFreq(uint8_t note);
  double NoteBank_GetScale(uint8_t note);

  /** Assign library attack id (0..255) to voice. */
  int NoteBank_SetWaveId(uint8_t note, uint16_t wave_id);
  uint16_t NoteBank_GetWaveId(uint8_t note);

  int NoteBank_SetShape(NoteBank_Shape_t shape, double param);
  NoteBank_Shape_t NoteBank_GetShape(void);
  double NoteBank_GetShapeParam(void);

  /** True while attack/sustain/release still sounds (incl. env release). */
  uint8_t NoteBank_IsActive(uint8_t note);
  uint8_t NoteBank_AnyActive(void);
  int32_t NoteBank_NextSample(void);

#ifdef __cplusplus
}
#endif

#endif /* __NOTE_BANK_H__ */
