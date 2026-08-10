/**
 ******************************************************************************
 * @file    body_bank.h
 * @brief   Legacy on-card int16 loops (unused by note_bank; pending removal).
 ******************************************************************************
 */

#ifndef __BODY_BANK_H__
#define __BODY_BANK_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

#include "attack_bank.h" /* SAMPLE_VOICES */

/** Library slots (same id space as attack heads for SAMPLE). */
#define BODY_BANK_COUNT SAMPLE_VOICES
/** Samples per sustain loop @ 48 kHz (~200 ms). */
#define BODY_BANK_LEN 9600u
#define BODY_BANK_BYTES (BODY_BANK_LEN * 2u)

  void BodyBank_Init(void);

  int BodyBank_Load(uint16_t wave_id, const uint8_t *data, uint32_t nbytes);
  int16_t *BodyBank_WritePtr(uint16_t wave_id);
  int BodyBank_Commit(uint16_t wave_id, uint32_t nsamp);
  uint8_t BodyBank_IsLoaded(uint16_t wave_id);
  uint32_t BodyBank_Length(uint16_t wave_id);

  /** Linear-interpolated sample at fractional index; wraps the loop. */
  int32_t BodyBank_SampleAt(uint16_t wave_id, double phase);

  void BodyBank_SetRootHz(uint16_t wave_id, float root_hz);
  float BodyBank_GetRootHz(uint16_t wave_id);

#ifdef __cplusplus
}
#endif

#endif /* __BODY_BANK_H__ */
