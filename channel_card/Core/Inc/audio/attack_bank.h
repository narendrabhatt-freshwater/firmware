/**
 ******************************************************************************
 * @file    attack_bank.h
 * @brief   AXI int16 attack heads (256 × ATTACK_BANK_LEN).
 *
 * Eight voices assign any loaded wave_id. Played on note-on before UAC
 * stream_ring sustain.
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

/** Stored heads (MIDI-sized bank). Independent of SAMPLE_VOICES. */
#define ATTACK_BANK_COUNT 256u
/** ~10.7 ms @ 48 kHz. 256 × 512 int16 = 256 KB AXI. */
#define ATTACK_BANK_LEN 512u
#define ATTACK_BANK_BYTES (ATTACK_BANK_LEN * 2u)
/** Source-index overlap of attack tail with body head. */
#define SAMPLE_CROSSFADE_LEN 32u
/** Join index when the head is committed at full length. */
#define SAMPLE_BODY_ORIGIN (ATTACK_BANK_LEN - SAMPLE_CROSSFADE_LEN)

  void AttackBank_Init(void);

  /**
   * @brief Replace one head with int16 LE bytes (2..ATTACK_BANK_BYTES).
   * @retval 0 ok
   * @retval -1 bad id / size / null
   */
  int AttackBank_Load(uint16_t wave_id, const uint8_t *data, uint32_t nbytes);

  /** Direct write pointer for CDC upload (ATTACK_BANK_LEN int16). */
  int16_t *AttackBank_WritePtr(uint16_t wave_id);

  /** Mark head present; nsamp is the real table length (not a hold-pad). */
  int AttackBank_Commit(uint16_t wave_id, uint32_t nsamp);

  uint8_t AttackBank_IsLoaded(uint16_t wave_id);

  /** Committed samples (0 if empty). Join is at this index, not ATTACK_BANK_LEN. */
  uint32_t AttackBank_GetLen(uint16_t wave_id);

  /** Loaded head count (0..256). */
  uint16_t AttackBank_LoadedCount(void);

  /** 256-bit mask, bit i = head i loaded. out must be 32 bytes. */
  void AttackBank_LoadedMask(uint8_t *out);

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
   * @brief Next Q31 sample for voice (rate-scaled). Last table sample
   *        when the head ends (not 0).
   */
  int32_t AttackBank_NextSample(uint8_t voice);

  /** Table sample as Q31; 0 if id/index invalid or unloaded. */
  int32_t AttackBank_SampleAt(uint16_t wave_id, uint32_t index);

  /** Contiguous Q15 head (ATTACK_BANK_LEN). NULL if id invalid. */
  const int16_t *AttackBank_Table(uint16_t wave_id);

#ifdef __cplusplus
}
#endif

#endif /* __ATTACK_BANK_H__ */
