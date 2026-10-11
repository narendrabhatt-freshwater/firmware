#ifndef CHANNEL_APP_BOARD_H
#define CHANNEL_APP_BOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hardware services used by the shared console and USB protocol code.
 * board_stm32.cpp implements them for the Channel Card; the host simulator
 * supplies its own. Keep this interface to plain functions and integers. */

/* ---- read time and mask interrupts -------------------------------------- */

/* Milliseconds since start; wraps at 2^32. */
uint32_t Board_Millis(void);

/* Mask every interrupt and return the previous mask for Board_IrqRestore(). */
uint32_t Board_IrqSave(void);
void Board_IrqRestore(uint32_t saved);

/* ---- drive leds and analog switches ------------------------------------- */

enum {
    BOARD_LED_R = 0,
    BOARD_LED_Y,
    BOARD_LED_RGB_R,
    BOARD_LED_RGB_G,
    BOARD_LED_RGB_B,
    BOARD_LED_COUNT
};

void Board_LedWrite(uint8_t led, uint8_t on);

enum {
    BOARD_SWITCH_HP = 0,
    BOARD_SWITCH_BP,
    BOARD_SWITCH_LP,
    BOARD_SWITCH_VCA,
    BOARD_SWITCH_BYPASS,
    BOARD_SWITCH_VCF,
    BOARD_SWITCH_SCF,
    BOARD_SWITCH_HP_CTL,
    BOARD_SWITCH_COUNT
};

/* Set the switch control pin level; polarity belongs to the caller. */
void Board_SwitchWrite(uint8_t sw, uint8_t high);

/* ---- share the rs485 bus ------------------------------------------------ */

/* Tri-state the driver-enable and transmit pins so another card can talk. */
void Board_Rs485Release(void);
/* Take the bus for one reply; pair every call with Board_Rs485Release(). */
void Board_Rs485Acquire(void);
/* Non-zero while no card is driving the bus. */
uint8_t Board_Rs485BusFree(void);
uint32_t Board_Rs485Baud(void);

/* ---- set dac attenuation ------------------------------------------------ */

/* Non-zero once a DAC is bound. Trim is in half-dB steps for channel '1'..'4'. */
uint8_t Board_DacReady(void);
void Board_DacSetTrim(char channel, uint8_t trim);

#ifdef __cplusplus
}
#endif

#endif
