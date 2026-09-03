/**
 ******************************************************************************
 * @file    attack_upload.h
 * @brief   CDC binary load of one signed-int8 attack head.
 ******************************************************************************
 */

#ifndef __ATTACK_UPLOAD_H__
#define __ATTACK_UPLOAD_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

  uint8_t AttackUpload_IsActive(void);
  int AttackUpload_Begin(uint16_t wave_id, uint32_t nbytes);
  uint32_t AttackUpload_Feed(const uint8_t *buf, uint32_t len);
  void AttackUpload_Abort(void);

#ifdef __cplusplus
}
#endif

#endif /* __ATTACK_UPLOAD_H__ */
