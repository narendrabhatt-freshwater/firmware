/**
 ******************************************************************************
 * @file    wave_upload.h
 * @brief   Chunked USB CDC binary load into WaveBank AXI slots.
 ******************************************************************************
 */

#ifndef __WAVE_UPLOAD_H__
#define __WAVE_UPLOAD_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

  /** Non-zero while CDC bytes should go to the uploader, not the line console. */
  uint8_t WaveUpload_IsActive(void);

  /**
   * @brief Begin receiving nbytes (even) into slot.
   * @retval 0 armed (caller should reply ok:ready)
   * @retval -1 bad args
   */
  int WaveUpload_Begin(uint8_t slot, uint32_t nbytes);

  /**
   * @brief Feed raw CDC bytes. May emit ok:chunk / ok:wave / err via USB_CDC_WriteStr.
   * @return Number of bytes consumed from buf (may be < len if session ends).
   */
  uint32_t WaveUpload_Feed(const uint8_t *buf, uint32_t len);

  /** Abort current session (e.g. disconnect). */
  void WaveUpload_Abort(void);

#ifdef __cplusplus
}
#endif

#endif /* __WAVE_UPLOAD_H__ */
