/**
 ******************************************************************************
 * @file    play_mode.c
 * @brief   notes vs wave playback mode switch.
 ******************************************************************************
 */

#include "play_mode.h"
#include "note_bank.h"

static PlayMode_t s_mode = PLAY_MODE_NOTES;

void PlayMode_Init(void)
{
  s_mode = PLAY_MODE_NOTES;
}

PlayMode_t PlayMode_Get(void)
{
  return s_mode;
}

int PlayMode_Set(PlayMode_t mode)
{
  if (mode != PLAY_MODE_NOTES && mode != PLAY_MODE_WAVE)
  {
    return -1;
  }

  NoteBank_PanicAll();
  s_mode = mode;
  return 0;
}
