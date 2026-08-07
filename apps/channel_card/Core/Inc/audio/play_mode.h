/**
 ******************************************************************************
 * @file    play_mode.h
 * @brief   Channel Card CH1 playback mode: notes (DDS) vs waveform (one-shot).
 ******************************************************************************
 */

#ifndef __PLAY_MODE_H__
#define __PLAY_MODE_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

  typedef enum
  {
    PLAY_MODE_NOTES = 0,
    PLAY_MODE_WAVE = 1
  } PlayMode_t;

  void PlayMode_Init(void);

  /** Current mode (default PLAY_MODE_NOTES). */
  PlayMode_t PlayMode_Get(void);

  /**
   * @brief Switch mode; stops all voices/waves and clears note-bank activity.
   * @retval 0 success
   * @retval -1 invalid mode
   */
  int PlayMode_Set(PlayMode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* __PLAY_MODE_H__ */
