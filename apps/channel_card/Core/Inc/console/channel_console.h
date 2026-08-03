/**
 ******************************************************************************
 * @file    channel_console.h
 * @brief   Channel Card RS485 + USB CDC console, cpuload probe, LED chaser.
 ******************************************************************************
 */

#ifndef __CHANNEL_CONSOLE_H__
#define __CHANNEL_CONSOLE_H__

#ifdef __cplusplus
extern "C"
{
#endif

  /** Tri-state RS485, default switches + session defaults, print ready banner. */
  void ChannelConsole_Init(void);

  /** Non-blocking RS485 RX poll + LED chaser step. */
  void ChannelConsole_Poll(void);

  /** USB CDC command entry (same parser as RS485). Declared here and used
   * from USB_APP — keep this symbol name stable. */
  void Console_ExecFromUSB(char *line);

#ifdef __cplusplus
}
#endif

#endif /* __CHANNEL_CONSOLE_H__ */
