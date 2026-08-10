/**
 ******************************************************************************
 * @file    body_upload.h
 * @brief   CDC session: bl → raw int16 LE → BodyBank_Commit.
 ******************************************************************************
 */

#ifndef __BODY_UPLOAD_H__
#define __BODY_UPLOAD_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

  uint8_t BodyUpload_IsActive(void);
  int BodyUpload_Begin(uint16_t wave_id, uint32_t nbytes);
  uint32_t BodyUpload_Feed(const uint8_t *buf, uint32_t len);
  void BodyUpload_Abort(void);

#ifdef __cplusplus
}
#endif

#endif /* __BODY_UPLOAD_H__ */
