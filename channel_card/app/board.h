/*
                                   __
                               ___  \  \
                          ___  \  \  \  \    _______
                     ___  \  \  \  \  \__\__/_____  \
                     \  \  \  \  \__\_______      \__\____
                      \  \  \  \     ___ \  \________  \__/
                       \  \  \__\___/_  \ \___/   \  \___
                        \  \    ____  \  \________ \ ___/
                         \__\__/ \  \  \____/  \  \
                                  \  \     ___  \  \___
                                   \__\___/ \  \ \____/
                                           \  \
                                              \  \___
                 __                 _          \ ___/  _
                / _|_ __  ___  ___ | |____      ____ _| |_  ___  _ __
               | |_| '_ |/ _ \/ __|| '_ \ \ /\ / / _` | __|/ _ \| '__|
               |  _| |  |  __/\__ \| | | \ V  V / (_| | |_|  __/| |
               |_| |_|   \___||___/|_| |_|\_/\_/ \__,_|\__|\___||_|

            (C) 2 0 2 6   F r e s h w a t e r   I n s t r u m e n t s
*/

#ifndef CHANNEL_APP_BOARD_H
#define CHANNEL_APP_BOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Platform services used by the shared application.
 * board_stm32.cpp implements them for the Channel Card; the host simulator
 * supplies its own. Keep this interface C-compatible. */

/* ---- read time and mask interrupts -------------------------------------- */

/* Milliseconds since start; wraps at 2^32. */
uint32_t get_board_time_ms(void);

/* Mask every interrupt and return the previous mask for restore_interrupt_state(). */
uint32_t disable_interrupts_and_save_state(void);
void restore_interrupt_state(uint32_t saved);
/* Publish sample metadata with the platform's memory-ordering barrier. */
void board_memory_barrier(void);
/* Optional profiling hooks, required only by builds that enable profiling. */
#if defined(CHANNEL_ENABLE_CYCLE_PROFILING)
void init_board_cycle_counter(void);
uint32_t get_board_cycle_count(void);
#endif

/* ---- drive leds and analog switches ------------------------------------- */

enum {
    BOARD_LED_R = 0,
    BOARD_LED_Y,
    BOARD_LED_COUNT
};

/* Logical status outputs; the backend owns GPIO mappings and polarity. */
void write_board_led(uint8_t led, uint8_t on);

/* Reset RGB control state without changing the physical startup output.
 * Colour and brightness are normalized [0, 1], validated by the caller.
 * Requests may originate in audio processing; the backend owns modulation
 * and synchronization. A request does not restart its modulation phase. */
void init_board_rgb_led(void);
int set_board_rgb_colour(float red, float green, float blue, float brightness);
/* Non-blocking main-loop service; simulation needs no physical modulation. */
void service_board_leds(void);

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

/* Enable/disable a logical switch; the backend owns electrical polarity. */
void write_board_switch(uint8_t sw, uint8_t enabled);

/* Clear output-driver overlap diagnostics; does not restart playback. */
void clear_audio_output_statistics(void);

/* ---- set dac attenuation ------------------------------------------------ */

/* Non-zero once a DAC is bound. Trim is in half-dB steps for channel '1'..'4'. */
uint8_t is_dac_ready(void);
void set_dac_channel_trim(char channel, uint8_t trim);

#ifdef __cplusplus
}
#endif

#endif
