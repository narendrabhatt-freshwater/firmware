/**
 ******************************************************************************
 * @file    channel_console.h
 * @brief   Channel Card RS485 + USB CDC console and status LEDs.
 *
 * One command parser serves both transports. RS485 uses card address prefixes
 * (`c:` / `*:` / bare) and tagged replies (`[C]ok` / `[C]err:<token>`).
 * Stable error tokens: syntax, range, unknown, rxdrop.
 *
 * Bring-up: call ChannelConsole_SetDacHandle() then ChannelConsole_Init()
 * after UART5 and the DAC handle are ready. Poll from the main loop after
 * servicing the USB task.
 ******************************************************************************
 */

#ifndef __CHANNEL_CONSOLE_H__
#define __CHANNEL_CONSOLE_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include "cs4304.h"

  /**
   * @brief Bind the CS4304 handle used by gain / trim console commands.
   * @param h DAC handle owned by main (non-NULL before Init / Poll).
   */
  void ChannelConsole_SetDacHandle(CS4304_HandleTypeDef *h);

  /**
   * @brief Tri-state RS485 idle, apply boot defaults, and emit the ready banner.
   * @note Also initializes note filter / bank / envelope cold state.
   */
  void ChannelConsole_Init(void);

  /**
   * @brief Non-blocking RS485 RX drain + LED chaser step.
   * @note Call every main-loop iteration; never blocks.
   */
  void ChannelConsole_Poll(void);

  /**
   * @brief Run one console command line that arrived over USB CDC.
   * @param line NUL-terminated command (mutated by the parser). May be NULL
   *             (no-op). Replies are routed to CDC for the duration of the call.
   */
  void Console_ExecFromUSB(char *line);

#ifdef __cplusplus
}
#endif

#endif /* __CHANNEL_CONSOLE_H__ */
