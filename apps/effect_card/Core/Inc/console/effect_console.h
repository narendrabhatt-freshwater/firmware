/**
 ******************************************************************************
 * @file    effect_console.h
 * @brief   Effect Card RS485 + USB CDC console, ADC helpers, LED flash.
 ******************************************************************************
 */

#ifndef __EFFECT_CONSOLE_H__
#define __EFFECT_CONSOLE_H__

#ifdef __cplusplus
extern "C" {
#endif

/** Tri-state RS485, power defaults, wake+config both ADCs. */
void EffectConsole_Init(void);

/** Non-blocking RS485 RX poll + LED alternating flash. */
void EffectConsole_Poll(void);

/** Tagged reply ([E] on RS485, raw on USB CDC when set by ExecFromUSB). */
void EffectConsole_Reply(const char *s);

/** USB CDC command entry (same parser as RS485). Declared here and used
 * from USB_APP — keep this symbol name stable. */
void Console_ExecFromUSB(char *line);

#ifdef __cplusplus
}
#endif

#endif /* __EFFECT_CONSOLE_H__ */
