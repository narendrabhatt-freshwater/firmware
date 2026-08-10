/**
 ******************************************************************************
 * @file    attack_bank.h
 * @brief   Per-slot int32 attack heads in AXI (ATTACK_BANK_LEN × 4 bytes).
 *
 * Played on note-on before UAC stream_ring sustain. Q31 for the voice path.
 ******************************************************************************
 */

#ifndef __ATTACK_BANK_H__
#define __ATTACK_BANK_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

/** SAMPLE voices n0..n7. */
#define SAMPLE_VOICES 8u

#define ATTACK_BANK_COUNT SAMPLE_VOICES
#define ATTACK_BANK_LEN 256u
#define ATTACK_BANK_BYTES (ATTACK_BANK_LEN * 4u)

  void AttackBank_Init(void);

  /**
   * @brief Replace one head with int32 LE bytes (exactly ATTACK_BANK_BYTES).
   * @retval 0 ok
   * @retval -1 bad id / size / null
   */
  int AttackBank_Load(uint16_t wave_id, const uint8_t *data, uint32_t nbytes);

  /** Direct write pointer for CDC upload (ATTACK_BANK_LEN int32). */
  int32_t *AttackBank_WritePtr(uint16_t wave_id);

  /** Mark head present after streaming into WritePtr. */
  int AttackBank_Commit(uint16_t wave_id);

  uint8_t AttackBank_IsLoaded(uint16_t wave_id);

  void AttackBank_SetRootHz(uint16_t wave_id, float root_hz);
  float AttackBank_GetRootHz(uint16_t wave_id);

  /**
   * @brief Start attack playhead for voice (0..7) from wave_id at phase_inc
   *        (samples of table per output sample). phase_inc from note_hz/root.
   */
  int AttackBank_NoteOn(uint8_t voice, uint16_t wave_id, float phase_inc);

  void AttackBank_Stop(uint8_t voice);
  void AttackBank_StopAll(void);

  /** Non-zero while attack samples remain for this voice. */
  uint8_t AttackBank_IsPlaying(uint8_t voice);

  /**
   * @brief Next Q31 sample for voice (rate-scaled), or 0 if finished/idle.
   */
  int32_t AttackBank_NextSample(uint8_t voice);

#ifdef __cplusplus
}
#endif

#endif /* __ATTACK_BANK_H__ */
